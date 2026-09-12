#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "esp_modem_link/tcp_client.h"
#include "esp_modem_link/websocket_client.h"
#include "platform/itask.h"
#include "protocol/http/http_request_builder.h"
#include "protocol/websocket/ws_frame.h"

namespace esp_modem_link::protocol {

// The scheme is read from the URL, so an address starting wss:// is all the
// client needs to ask for a socket under TLS. WebSocketClient has nowhere to
// put a TlsConfig, which is why this takes one flag rather than the HTTP and
// MQTT engines' (tls, config) pair.
using WsTransportFactory =
    std::function<Result<std::unique_ptr<TcpClient>>(bool tls)>;

// ws:// and wss:// resolved through the same URL splitter the HTTP engine uses,
// with the scheme rewritten to its HTTP equivalent. A WebSocket handshake is an
// HTTP request, so the authority, port and path rules are the same ones, and a
// second implementation of them would be a second set of bugs.
Result<ParsedUrl> ParseWebSocketUrl(std::string_view url);

// RFC 6455 over any TcpClient, so every module gets a WebSocket whatever its
// firmware offers.
//
// Three things about this class are worth knowing before reading it:
//
// Callbacks run on the transport's receive thread - the AT layer's for a
// cellular module - and also on the maintenance task, which is where a
// connection that fails its heartbeat is noticed and where OnConnected fires
// again after an automatic reconnect. They are never called with an internal
// lock held, so a callback may send or close, but it must not assume it is on
// the thread that called Connect().
//
// One task does both jobs: while the connection is open it sends the heartbeat
// and watches for the answer, and while it is down it retries with a backoff.
// It is started once a connection is established and runs until Close(), so a
// connection that drops and comes back never leaves the application without a
// heartbeat.
//
// OnMessage carries the bytes and nothing else, because DataCallback has no
// room for a flag: a text message and a binary one arrive the same way here,
// even though Send() and SendFragment() let a sender say which it is sending.
class SoftwareWsClient : public WebSocketClient {
 public:
  // The handshake wait is a constructor argument rather than a setter because
  // WebSocketClient has no SetTimeout, and a caller only ever wants to shorten
  // it.
  explicit SoftwareWsClient(
      WsTransportFactory factory,
      std::chrono::milliseconds handshake_timeout = std::chrono::seconds(10));
  ~SoftwareWsClient() override;

  void SetHeader(std::string_view key, std::string_view value) override;

  // An interval of zero - the default - turns the heartbeat off. The timeout is
  // how long the peer has to answer a PING before the connection is treated as
  // dead; zero means the interval is used instead, so a caller that names only
  // an interval still gets a deadline.
  void SetHeartbeat(std::chrono::seconds interval,
                    std::chrono::seconds timeout) override;

  // With reconnection on, a connection that drops or fails its heartbeat is
  // re-established with a backoff, and OnConnected fires again when it is. The
  // wait starts at a second and doubles to half a minute; max_retries bounds
  // how many consecutive attempts are made, and -1 means no bound. Reconnection
  // is off until this is called.
  void SetAutoReconnect(bool enable, int max_retries = -1) override;

  // Returns once the upgrade has been accepted and the server's
  // Sec-WebSocket-Accept has been checked against the key that was sent, so a
  // successful return means a usable connection.
  Result<> Connect(std::string_view url) override;

  // Sends a close frame and waits briefly for the peer's, then reports how the
  // connection ended through OnDisconnected: the peer's code when it answered,
  // and the one asked for here when it did not.
  void Close(WebSocketCloseCode code = WebSocketCloseCode::kNormal,
             std::string_view reason = "") override;

  Result<> Send(std::string_view data, bool binary = false) override;

  // Sends one frame of a message the caller is splitting up itself. The first
  // frame of a message carries the text or binary kind and fin false; the rest
  // are continuations, which is why `binary` is not consulted once a message is
  // under way. A sequence of fragments is one sequence of calls: two threads
  // fragmenting at the same time would interleave their messages.
  Result<> SendFragment(const void* data,
                        size_t len,
                        bool binary,
                        bool fin) override;

  // Sends a ping and starts the heartbeat's clock for its pong. OnPong fires
  // for every pong, the heartbeat's included.
  void Ping(std::string_view payload = "") override;

 private:
  enum class State {
    kDisconnected,
    kHandshaking,
    kOpen,
  };

  // Brings up a transport and completes the handshake, leaving the client open
  // on success. Called from Connect() on the caller's thread and from the
  // maintenance task when reconnecting.
  Result<> Establish(std::string_view url);

  // Transport callbacks, invoked from the receive thread.
  void OnTransportData(std::string_view data);
  void OnTransportClosed();

  // Takes in whatever arrived and consumes the handshake if it is now complete.
  // False when the buffer limit was passed, which has already been reported.
  bool QueueInbound(std::string_view data);

  // Takes frames out of the receive buffer and handles them until one is
  // incomplete or the buffer runs out.
  void DrainFrames();

  // Consumes a handshake response whose head is now complete. Caller holds
  // mutex_.
  void ConsumeHandshakeLocked();

  void HandleFrame(const WsFrame& frame);
  void HandleDataFrame(const WsFrame& frame);
  void HandleControlFrame(const WsFrame& frame);
  void HandleCloseFrame(std::string_view payload);

  // Sends one frame. Masked, as every client frame must be, with a key drawn
  // fresh for it.
  Result<> SendFrame(WsOpcode opcode, std::string_view payload, bool fin);

  // Sends a ping and records when it went out. The heartbeat's pings and the
  // caller's share the clock, because the timeout rule is about whether the
  // peer is answering at all and cannot tell the two apart.
  Result<> SendPing(std::string_view payload);

  // Sends a close frame if there is an open connection to send it over, and
  // remembers that one has gone out. False when there was nothing to send it
  // over: a handshake that never completed has no WebSocket to close politely.
  bool SendCloseFrame(uint16_t code, std::string_view reason);

  // An error on the wire: reports it, says goodbye with the close frame the
  // specification pairs with it, and tears down.
  void Fail(const NetworkError& error, uint16_t close_code,
            std::string_view reason);

  // Closes the transport and tells the application, if it was ever told the
  // connection was open. Never joins the maintenance task - it may be that task
  // that got here - so it stops the loop and lets the join happen in
  // StopMaintain().
  void Teardown(uint16_t code, std::string_view reason);

  void StartMaintain();
  void StopMaintain();
  void MaintainLoop();
  // Sleeps in ticks, so a stop request is noticed within one rather than after
  // a backoff that can be half a minute long.
  void WaitInterruptible(std::chrono::milliseconds total);

  void ReportError(const NetworkError& error);

  WsTransportFactory factory_;
  const std::chrono::milliseconds handshake_timeout_;

  // Guards every member the receive thread and the maintenance task touch.
  // Never held across a call into the transport: a transport that answers
  // inside Send() would otherwise re-enter the receive path and deadlock.
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  State state_ = State::kDisconnected;

  std::shared_ptr<TcpClient> transport_;
  std::string rx_buffer_;

  std::vector<std::pair<std::string, std::string>> headers_;
  std::string url_;
  bool user_closed_ = false;

  std::chrono::seconds heartbeat_interval_{0};
  std::chrono::seconds heartbeat_timeout_{0};
  bool reconnect_enabled_ = false;
  int max_retries_ = -1;
  int reconnect_attempt_ = 0;
  // Set when the retries have run out, so the giving up is reported once rather
  // than on every tick that follows it.
  bool reconnect_exhausted_ = false;

  // Handshake state. handshake_key_ is kept because the accept value the server
  // sends is only checkable against the key that actually went out.
  std::string handshake_key_;
  bool handshake_done_ = false;
  std::optional<NetworkError> handshake_error_;

  // A message being reassembled from fragments: its kind was fixed by the frame
  // that started it.
  bool fragment_open_ = false;
  std::string fragment_payload_;

  // True once a close frame has gone out, so the peer's reply is not answered
  // with a second one.
  bool close_sent_ = false;

  std::unique_ptr<platform::ITask> maintain_task_;
  std::thread::id maintain_thread_;
  std::atomic<bool> maintain_running_{false};
  // Milliseconds on the platform clock. Idle time is measured from the last
  // frame of any kind, which is what the specification asks a client to do.
  std::atomic<long long> last_sent_ms_{0};
  // Zero when no ping is outstanding.
  std::atomic<long long> ping_sent_ms_{0};
};

}  // namespace esp_modem_link::protocol
