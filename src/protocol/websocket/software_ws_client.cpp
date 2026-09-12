#include "protocol/websocket/software_ws_client.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "platform/random.h"
#include "platform/time.h"
#include "protocol/websocket/ws_handshake.h"

namespace esp_modem_link::protocol {
namespace {

// How often the maintenance task wakes. Short enough that a heartbeat goes out
// near its deadline and that a stop request is noticed promptly, long enough
// not to matter.
constexpr std::chrono::milliseconds kMaintainTick{200};

// The most a message may be, whether it arrives in one frame or many, and the
// most that may sit unconsumed in the receive buffer. A peer promising more
// than this is asking the device to hold something it should not, and 1009 is
// the code the specification provides for saying so.
constexpr size_t kMaxMessageBytes = 64 * 1024;

// How long Close() waits for the peer's half of the close handshake before
// dropping the socket. A close reply arrives within one round trip, and a
// cellular round trip is the slowest this will meet.
constexpr std::chrono::milliseconds kCloseWait{2000};

// Reconnection waits a second before the first attempt and doubles from there,
// which is short enough to come back from a dropped bearer and long enough not
// to hammer a server that is down.
constexpr std::chrono::milliseconds kReconnectInitialDelay{1000};
constexpr std::chrono::milliseconds kReconnectMaxDelay{30000};

// A control frame's payload is a two-byte status code followed by as much of the
// reason as fits in the 125 bytes a control frame may carry.
constexpr size_t kMaxCloseReason = kMaxControlPayload - 2;

// The client whose delivery is being drained on this thread, if any. It is what
// keeps a transport that answers from inside Send() from driving the receive
// path back into itself; see OnTransportData.
thread_local const SoftwareWsClient* draining_client = nullptr;

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::chrono::milliseconds BackoffFor(int attempt) {
  auto delay = kReconnectInitialDelay;
  for (int i = 0; i < attempt && delay < kReconnectMaxDelay; ++i) {
    delay *= 2;
  }
  return std::min(delay, kReconnectMaxDelay);
}

// Trims the reason to what a close frame can hold. The cut backs off to the
// last character that ended, because cutting a multi-byte character in half
// would put invalid UTF-8 in a field the specification says is text.
std::string_view FitReason(std::string_view reason) {
  if (reason.size() <= kMaxCloseReason) return reason;
  size_t end = kMaxCloseReason;
  while (end > 0 && (static_cast<uint8_t>(reason[end]) & 0xC0) == 0x80) --end;
  return reason.substr(0, end);
}

// The payload of a close frame: a status code and a reason, or nothing at all
// (5.5.1). 1005 is how the specification names "no status", and it is expressed
// by an empty payload rather than by the number, which the peer would have to
// reject as undefined.
std::string ClosePayload(uint16_t code, std::string_view reason) {
  if (code == static_cast<uint16_t>(WebSocketCloseCode::kNoStatus)) return {};
  std::string payload;
  payload.push_back(static_cast<char>((code >> 8) & 0xFF));
  payload.push_back(static_cast<char>(code & 0xFF));
  payload.append(FitReason(reason));
  return payload;
}

// The value of a header in a caller's list, matched case-insensitively as RFC
// 9110 requires. Empty when the caller set none.
std::string HeaderValue(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view name) {
  const std::string lowered = ToLower(name);
  for (const auto& [existing_name, existing_value] : headers) {
    if (ToLower(existing_name) == lowered) return existing_value;
  }
  return {};
}

}  // namespace

Result<ParsedUrl> ParseWebSocketUrl(std::string_view url) {
  // Rewritten rather than reimplemented: the splitter the HTTP engine uses
  // knows the authority, port and path rules, and ws and http agree on all
  // three. A scheme that is neither ws nor wss is refused here rather than
  // reaching the request builder, which would send it as though it were one.
  std::string rewritten;
  if (url.rfind("ws://", 0) == 0) {
    rewritten = "http://";
    rewritten.append(url.substr(5));
  } else if (url.rfind("wss://", 0) == 0) {
    rewritten = "https://";
    rewritten.append(url.substr(6));
  } else {
    return std::unexpected(NetworkError(NetworkErrc::kInvalidArgument, 0,
                                        "URL must start with ws:// or wss://"));
  }
  return ParseUrl(rewritten);
}

SoftwareWsClient::SoftwareWsClient(WsTransportFactory factory,
                                  std::chrono::milliseconds handshake_timeout)
    : factory_(std::move(factory)), handshake_timeout_(handshake_timeout) {}

SoftwareWsClient::~SoftwareWsClient() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    user_closed_ = true;
  }
  StopMaintain();
  // Saying goodbye is best effort here and is not waited on: a destructor that
  // sat for two seconds hoping a peer would answer would be a worse neighbour
  // than one that closes the socket.
  SendCloseFrame(static_cast<uint16_t>(WebSocketCloseCode::kNormal), "");
  // 1006 rather than 1000: the socket is dropped rather than waited on, so the
  // close handshake did not finish, whatever was meant by starting it.
  Teardown(static_cast<uint16_t>(WebSocketCloseCode::kAbnormalClosure), "");
}

void SoftwareWsClient::SetHeader(std::string_view key, std::string_view value) {
  std::lock_guard<std::mutex> lock(mutex_);
  // One entry per header name, so setting a name twice replaces it rather than
  // sending it twice, which is what the request builder would otherwise have to
  // sort out for itself.
  for (auto& [existing_key, existing_value] : headers_) {
    if (ToLower(existing_key) == ToLower(key)) {
      existing_value = std::string(value);
      return;
    }
  }
  headers_.emplace_back(std::string(key), std::string(value));
}

void SoftwareWsClient::SetHeartbeat(std::chrono::seconds interval,
                                    std::chrono::seconds timeout) {
  std::lock_guard<std::mutex> lock(mutex_);
  heartbeat_interval_ = interval;
  heartbeat_timeout_ = timeout;
}

void SoftwareWsClient::SetAutoReconnect(bool enable, int max_retries) {
  std::lock_guard<std::mutex> lock(mutex_);
  reconnect_enabled_ = enable;
  max_retries_ = max_retries;
  reconnect_attempt_ = 0;
  reconnect_exhausted_ = false;
}

Result<> SoftwareWsClient::Connect(std::string_view url) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kDisconnected) {
      return std::unexpected(NetworkError(NetworkErrc::kAlreadyConnected, 0,
                                          "already connected"));
    }
    url_ = std::string(url);
    // A caller that connects again is asking for a connection to be kept, which
    // is the opposite of what a close asked for.
    user_closed_ = false;
    reconnect_attempt_ = 0;
    reconnect_exhausted_ = false;
  }

  auto established = Establish(url);
  if (!established.has_value()) {
    return std::unexpected(established.error());
  }

  StartMaintain();
  // Outside the lock, and after the connection is genuinely usable: a callback
  // that sends has to find a connection that is already open.
  if (on_connected_) on_connected_();
  return {};
}

Result<> SoftwareWsClient::Establish(std::string_view url) {
  auto parsed = ParseWebSocketUrl(url);
  if (!parsed.has_value()) {
    return std::unexpected(parsed.error());
  }

  std::string key;
  std::vector<std::pair<std::string, std::string>> headers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kDisconnected) {
      return std::unexpected(NetworkError(NetworkErrc::kAlreadyConnected, 0,
                                          "already connected"));
    }
    headers = headers_;
    // A caller that set the key itself is taken at its word, and the accept
    // value is then checked against the key that actually went out.
    key = HeaderValue(headers, "Sec-WebSocket-Key");
    if (key.empty()) key = MakeSecWebSocketKey();
    handshake_key_ = key;
    handshake_done_ = false;
    handshake_error_.reset();
    rx_buffer_.clear();
    fragment_open_ = false;
    fragment_payload_.clear();
    close_sent_ = false;
    state_ = State::kHandshaking;
  }

  auto created = factory_(parsed->tls);
  if (!created.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::kDisconnected;
    return std::unexpected(created.error());
  }
  // Registered before the first byte is written: a transport that answers
  // inside Send() must not be able to reach a client that is not listening yet.
  auto transport = std::shared_ptr<TcpClient>(std::move(created.value()));
  transport->OnData([this](std::string_view data) { OnTransportData(data); });
  transport->OnDisconnected([this]() { OnTransportClosed(); });

  auto opened = transport->Connect(parsed->host, parsed->port);
  if (!opened.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::kDisconnected;
    return std::unexpected(opened.error());
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    transport_ = transport;
  }

  // Sent through the local handle: the request goes out before there is
  // anything for SendFrame() to find in transport_, and the handshake is not a
  // frame in any case.
  const std::string request = BuildHandshakeRequest(*parsed, key, headers);
  auto sent = transport->Send(request.data(), request.size());
  if (!sent.has_value()) {
    Teardown(static_cast<uint16_t>(WebSocketCloseCode::kAbnormalClosure), "");
    return std::unexpected(sent.error());
  }

  std::unique_lock<std::mutex> lock(mutex_);
  const bool answered =
      cv_.wait_for(lock, handshake_timeout_, [this] { return handshake_done_; });
  if (!answered) {
    lock.unlock();
    // No close frame: the upgrade never completed, so there is no WebSocket to
    // close politely, and the socket is simply dropped.
    Teardown(static_cast<uint16_t>(WebSocketCloseCode::kAbnormalClosure), "");
    return std::unexpected(
        NetworkError::Timeout("the server did not answer the upgrade"));
  }
  if (handshake_error_.has_value()) {
    const NetworkError error = *handshake_error_;
    lock.unlock();
    Teardown(static_cast<uint16_t>(WebSocketCloseCode::kAbnormalClosure), "");
    return std::unexpected(error);
  }
  state_ = State::kOpen;
  connected_ = true;
  // The heartbeat measures idleness from here rather than from the zero the
  // clock starts at, so a connection that is established and then left quiet
  // gets its full interval before the first ping rather than an immediate one.
  last_sent_ms_ = platform::Now().count();
  lock.unlock();
  return {};
}

void SoftwareWsClient::Close(WebSocketCloseCode code, std::string_view reason) {
  const uint16_t close_code = static_cast<uint16_t>(code);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    user_closed_ = true;
  }
  // The maintenance task is not joined from inside itself: a callback that runs
  // on it - OnConnected after an automatic reconnect - may legitimately close.
  StopMaintain();

  // 1006 describes a connection that ended without a close handshake, so it is
  // not something a caller can ask for. Dropping the socket is what an abnormal
  // closure is, and that is what happens instead.
  if (code == WebSocketCloseCode::kAbnormalClosure) {
    ReportError(NetworkError(
        NetworkErrc::kInvalidArgument, 1006,
        "1006 describes how a connection ended and cannot be requested"));
    Teardown(close_code, reason);
    return;
  }

  if (!SendCloseFrame(close_code, reason)) {
    // Nothing was open, so there is nothing to close and no one to tell.
    return;
  }

  // Wait for the peer's half of the handshake, so the close is a conversation
  // rather than a dropped socket - but only briefly, and a peer that answers
  // tears down through HandleCloseFrame() and reports its own code, which is
  // what makes the wait end early.
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, kCloseWait, [this] { return state_ != State::kOpen; });
  lock.unlock();
  Teardown(close_code, reason);
}

Result<> SoftwareWsClient::Send(std::string_view data, bool binary) {
  return SendFragment(data.data(), data.size(), binary, true);
}

Result<> SoftwareWsClient::SendFragment(const void* data, size_t len, bool binary,
                                        bool fin) {
  if (data == nullptr && len != 0) {
    return std::unexpected(
        NetworkError(NetworkErrc::kInvalidArgument, 0, "no data to send"));
  }

  WsOpcode opcode = WsOpcode::kText;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kOpen) {
      return std::unexpected(
          NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
    }
    // A continuation frame carries no kind: which one the message is was fixed
    // by the frame that started it, so `binary` is not consulted here.
    opcode = fragment_open_ ? WsOpcode::kContinuation
                            : (binary ? WsOpcode::kBinary : WsOpcode::kText);
    fragment_open_ = !fin;
  }

  const std::string payload =
      len == 0 ? std::string()
               : std::string(static_cast<const char*>(data), len);
  return SendFrame(opcode, payload, fin);
}

Result<> SoftwareWsClient::SendFrame(WsOpcode opcode, std::string_view payload,
                                     bool fin) {
  // Masked, with a key drawn for this frame. A client frame that is not masked
  // is one a conforming server closes the connection over (5.1).
  auto encoded =
      EncodeFrame(opcode, payload, fin, platform::RandomUint32());
  if (!encoded.has_value()) {
    return std::unexpected(encoded.error());
  }

  std::shared_ptr<TcpClient> transport;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    transport = transport_;
  }
  if (!transport) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
  }

  // The lock is not held here, so this may run concurrently with the receive
  // thread, which answers a ping from it. One frame per call is one AT command
  // at the layer below, which is what keeps two frames from interleaving.
  auto result = transport->Send(encoded->data(), encoded->size());
  if (!result.has_value()) {
    return std::unexpected(result.error());
  }
  last_sent_ms_ = platform::Now().count();
  return {};
}

Result<> SoftwareWsClient::SendPing(std::string_view payload) {
  auto sent = SendFrame(WsOpcode::kPing, payload, true);
  if (!sent.has_value()) {
    return std::unexpected(sent.error());
  }
  // Recorded for the caller's pings and the heartbeat's alike: the timeout rule
  // is about whether the peer is answering at all, and it cannot tell them
  // apart.
  ping_sent_ms_ = platform::Now().count();
  return {};
}

void SoftwareWsClient::Ping(std::string_view payload) {
  if (payload.size() > kMaxControlPayload) {
    ReportError(NetworkError(NetworkErrc::kInvalidArgument, 0,
                             "a ping payload cannot exceed 125 bytes"));
    return;
  }
  auto sent = SendPing(payload);
  if (!sent.has_value()) {
    ReportError(sent.error());
  }
}

bool SoftwareWsClient::SendCloseFrame(uint16_t code, std::string_view reason) {
  std::shared_ptr<TcpClient> transport;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kOpen) return false;
    // Set before the frame goes out, so the peer's reply arriving on the
    // receive thread is not answered with a second close frame.
    close_sent_ = true;
    transport = transport_;
  }
  if (!transport) return false;

  auto encoded =
      EncodeFrame(WsOpcode::kClose, ClosePayload(code, reason), true,
                  platform::RandomUint32());
  if (encoded.has_value()) {
    transport->Send(encoded->data(), encoded->size());
    last_sent_ms_ = platform::Now().count();
  }
  return true;
}

void SoftwareWsClient::OnTransportData(std::string_view data) {
  if (!QueueInbound(data)) return;

  // A transport that answers from inside Send() - the test mock does, and a
  // driver that pumped its own receive path synchronously would - re-enters
  // here from a handler that is itself sending, and recursing through Fail()
  // and SendCloseFrame() that way has no bound. The bytes are queued either
  // way, so the delivery already on this thread is left to drain them.
  if (draining_client == this) return;
  draining_client = this;
  DrainFrames();
  draining_client = nullptr;
}

bool SoftwareWsClient::QueueInbound(std::string_view data) {
  bool overflowed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    rx_buffer_.append(data);
    if (rx_buffer_.size() > kMaxMessageBytes) {
      rx_buffer_.clear();
      overflowed = true;
    } else if (!handshake_done_) {
      ConsumeHandshakeLocked();
    }
  }
  if (!overflowed) return true;

  // Reported outside the lock: the close frame this sends goes through the
  // transport, and a transport that answers inside Send() would re-enter here
  // with the lock held.
  Fail(NetworkError(NetworkErrc::kBufferOverflow, 0,
                    "the inbound stream exceeds the buffer limit"),
       static_cast<uint16_t>(WebSocketCloseCode::kMessageTooBig),
       "message too big");
  return false;
}

void SoftwareWsClient::DrainFrames() {
  // Frames are taken under the lock and handled without it, because a handler
  // may answer a ping or send from a callback, and both take the lock.
  for (;;) {
    WsFrame frame;
    std::string violation;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!handshake_done_) return;
      const WsFrameRead read = TakeFrame(rx_buffer_);
      if (read.status == WsFrameStatus::kIncomplete) return;
      if (read.status == WsFrameStatus::kInvalid) {
        violation = read.reason;
      } else {
        frame = std::move(read.frame);
      }
    }
    if (!violation.empty()) {
      Fail(NetworkError(NetworkErrc::kProtocolError, 0, violation),
           static_cast<uint16_t>(WebSocketCloseCode::kProtocolError),
           violation);
      return;
    }
    HandleFrame(frame);
  }
}

void SoftwareWsClient::ConsumeHandshakeLocked() {
  const size_t end = rx_buffer_.find("\r\n\r\n");
  if (end == std::string::npos) return;

  // Everything through the blank line is the response head; whatever follows it
  // in the same arrival is already frame bytes.
  const std::string head = rx_buffer_.substr(0, end + 4);
  rx_buffer_.erase(0, end + 4);
  handshake_done_ = true;

  auto parsed = ParseHandshakeResponse(head);
  if (!parsed.has_value()) {
    handshake_error_ = parsed.error();
  } else if (parsed->status != 101) {
    handshake_error_ =
        NetworkError(NetworkErrc::kWsHandshakeFailed, parsed->status,
                     "the server answered the upgrade with " +
                         std::to_string(parsed->status));
  } else if (parsed->accept != ComputeSecWebSocketAccept(handshake_key_)) {
    // The point of the accept value: it proves the server read the key that was
    // sent. A proxy answering from its own cache cannot produce it, and a
    // server that did not read the key is not one to speak WebSocket with.
    handshake_error_ = NetworkError(
        NetworkErrc::kWsHandshakeFailed, 0,
        "the server's Sec-WebSocket-Accept does not match the key that was "
        "sent");
  }
  cv_.notify_all();
}

void SoftwareWsClient::OnTransportClosed() {
  // The link is already gone, so there is no close frame to send over it, and
  // 1006 is the code for exactly that.
  Teardown(static_cast<uint16_t>(WebSocketCloseCode::kAbnormalClosure), "");
}

void SoftwareWsClient::HandleFrame(const WsFrame& frame) {
  if (IsControlOpcode(frame.opcode)) {
    HandleControlFrame(frame);
    return;
  }
  HandleDataFrame(frame);
}

void SoftwareWsClient::HandleDataFrame(const WsFrame& frame) {
  std::string message;
  std::string violation;
  bool too_big = false;
  bool deliver = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frame.opcode == WsOpcode::kContinuation && !fragment_open_) {
      // A continuation with nothing to continue: the frames of a message are
      // counted from the one that opened it, and this one has no such frame.
      violation = "a continuation frame arrived with no message to continue";
    } else if (frame.opcode != WsOpcode::kContinuation && fragment_open_) {
      violation = "a new message started before the last one finished";
    } else if (frame.opcode == WsOpcode::kContinuation) {
      fragment_payload_.append(frame.payload);
      if (fragment_payload_.size() > kMaxMessageBytes) {
        violation = "a fragmented message exceeds the size limit";
        too_big = true;
      } else if (frame.fin) {
        message = std::move(fragment_payload_);
        fragment_payload_.clear();
        fragment_open_ = false;
        deliver = true;
      }
    } else if (frame.fin) {
      message = frame.payload;
      deliver = true;
    } else {
      // The first frame of a message the caller is splitting up: nothing is
      // delivered until the frame that sets fin arrives.
      fragment_open_ = true;
      fragment_payload_ = frame.payload;
    }
  }

  if (!violation.empty()) {
    Fail(NetworkError(too_big ? NetworkErrc::kBufferOverflow
                              : NetworkErrc::kProtocolError,
                      0, violation),
         static_cast<uint16_t>(too_big ? WebSocketCloseCode::kMessageTooBig
                                       : WebSocketCloseCode::kProtocolError),
         too_big ? "message too big" : violation);
    return;
  }
  if (deliver && on_message_) on_message_(message);
}

void SoftwareWsClient::HandleControlFrame(const WsFrame& frame) {
  switch (frame.opcode) {
    case WsOpcode::kPing:
      // Echoed with the same payload, which is what lets the peer match the
      // pong to the ping it sent (5.5.3).
      SendFrame(WsOpcode::kPong, frame.payload, true);
      break;
    case WsOpcode::kPong:
      ping_sent_ms_ = 0;
      if (on_pong_) on_pong_(frame.payload);
      break;
    case WsOpcode::kClose:
      HandleCloseFrame(frame.payload);
      break;
    default:
      // Nothing else reaches here: the frame codec rejects the reserved
      // opcodes, and the data opcodes were handled above.
      break;
  }
}

void SoftwareWsClient::HandleCloseFrame(std::string_view payload) {
  if (payload.size() == 1) {
    Fail(NetworkError(NetworkErrc::kProtocolError, 0,
                      "a close frame carries a single byte, which cannot be a "
                      "status code"),
         static_cast<uint16_t>(WebSocketCloseCode::kProtocolError),
         "malformed close frame");
    return;
  }

  uint16_t code = static_cast<uint16_t>(WebSocketCloseCode::kNoStatus);
  std::string reason;
  if (payload.size() >= 2) {
    code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) |
                                 static_cast<uint8_t>(payload[1]));
    reason = std::string(payload.substr(2));
  }

  bool already_sent = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    already_sent = close_sent_;
    close_sent_ = true;
  }
  // Answered only when the peer closed first. A close frame sent in reply is
  // the other half of the handshake, and a peer answering a close this client
  // began has already had its half.
  if (!already_sent) {
    SendFrame(WsOpcode::kClose, payload, true);
  }
  Teardown(code, reason);
}

void SoftwareWsClient::Fail(const NetworkError& error, uint16_t close_code,
                            std::string_view reason) {
  ReportError(error);
  SendCloseFrame(close_code, reason);
  Teardown(close_code, reason);
}

void SoftwareWsClient::Teardown(uint16_t code, std::string_view reason) {
  std::shared_ptr<TcpClient> transport;
  bool was_open = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only a connection the application was told about is worth telling about
    // again: a Connect() that failed reports its own error, and a second
    // notification would be noise.
    was_open = state_ == State::kOpen && transport_ != nullptr;
    state_ = State::kDisconnected;
    transport = std::move(transport_);
    rx_buffer_.clear();
    fragment_open_ = false;
    fragment_payload_.clear();
    close_sent_ = false;
    ping_sent_ms_ = 0;
  }
  cv_.notify_all();

  if (transport) transport->Disconnect();
  if (was_open) {
    connected_ = false;
    if (on_disconnected_) on_disconnected_(code, reason);
  }
}

void SoftwareWsClient::StartMaintain() {
  StopMaintain();
  maintain_running_ = true;
  maintain_task_ =
      platform::CreateTask("ws_maintain", [this]() { MaintainLoop(); }, 4096, 4);
  maintain_task_->Start();
}

void SoftwareWsClient::StopMaintain() {
  maintain_running_ = false;
  if (!maintain_task_) return;
  // A task joining itself would deadlock, and a callback running on this task
  // may legitimately call Close(). The loop notices the flag within a tick and
  // ends; the join then happens here the next time, or in the destructor.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::this_thread::get_id() == maintain_thread_) return;
  }
  maintain_task_->Stop();
  maintain_task_.reset();
}

void SoftwareWsClient::WaitInterruptible(std::chrono::milliseconds total) {
  for (auto waited = std::chrono::milliseconds(0);
       waited < total && maintain_running_; waited += kMaintainTick) {
    platform::Sleep(kMaintainTick);
  }
}

void SoftwareWsClient::MaintainLoop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    maintain_thread_ = std::this_thread::get_id();
  }

  while (maintain_running_) {
    platform::Sleep(kMaintainTick);
    if (!maintain_running_) break;

    State state = State::kDisconnected;
    bool reconnect = false;
    bool user_closed = false;
    int max_retries = 0;
    int attempt = 0;
    std::chrono::seconds interval{0};
    std::chrono::seconds timeout{0};
    std::string url;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state = state_;
      reconnect = reconnect_enabled_;
      user_closed = user_closed_;
      max_retries = max_retries_;
      attempt = reconnect_attempt_;
      interval = heartbeat_interval_;
      timeout = heartbeat_timeout_;
      url = url_;
    }

    if (state == State::kOpen) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        reconnect_attempt_ = 0;
      }
      // An interval of zero is how the heartbeat is turned off.
      if (interval.count() <= 0) continue;

      const auto seconds_to_ms = [](std::chrono::seconds value) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(value)
            .count();
      };
      const long long now = platform::Now().count();
      const long long ping_sent = ping_sent_ms_.load();
      if (ping_sent != 0) {
        // A caller that named only an interval still gets a deadline: waiting
        // for a pong forever would leave the connection looking alive.
        const long long deadline =
            seconds_to_ms(timeout.count() > 0 ? timeout : interval);
        if (now - ping_sent >= deadline) {
          ReportError(NetworkError(NetworkErrc::kTimeout, 0,
                                   "the peer did not answer a ping"));
          // 1006, because the connection is being abandoned without a close
          // handshake - which is what that code means.
          Teardown(
              static_cast<uint16_t>(WebSocketCloseCode::kAbnormalClosure),
              "no pong from the peer");
        }
        continue;
      }
      if (now - last_sent_ms_.load() >= seconds_to_ms(interval)) {
        auto sent = SendPing("");
        if (!sent.has_value()) ReportError(sent.error());
      }
      continue;
    }

    if (state != State::kDisconnected || !reconnect || user_closed) continue;

    if (max_retries >= 0 && attempt >= max_retries) {
      // Reported once. The attempt count is not reset here, so this branch is
      // not entered again until a connection succeeds or the caller asks for
      // one.
      if (!reconnect_exhausted_) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          reconnect_exhausted_ = true;
        }
        ReportError(NetworkError(
            NetworkErrc::kConnectFailed, attempt,
            "giving up after " + std::to_string(attempt) +
                " reconnection attempts"));
      }
      continue;
    }

    WaitInterruptible(BackoffFor(attempt));
    if (!maintain_running_) break;

    auto established = Establish(url);
    if (!established.has_value()) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        reconnect_attempt_ = attempt + 1;
      }
      ReportError(established.error());
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      reconnect_attempt_ = 0;
      reconnect_exhausted_ = false;
    }
    if (on_connected_) on_connected_();
  }
}

void SoftwareWsClient::ReportError(const NetworkError& error) {
  if (on_error_) on_error_(error);
}

}  // namespace esp_modem_link::protocol
