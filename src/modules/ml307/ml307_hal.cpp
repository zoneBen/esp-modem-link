#include "modules/ml307/ml307_hal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "at_parser/at_parser.h"
#include "hal/module_registry.h"

namespace esp_modem_link::modules::ml307 {

using at_channel::IAtChannel;
using at_parser::ParseInt;
using at_parser::SplitCsv;
using at_parser::StripKeyPrefix;
using at_parser::StripQuotes;

namespace {

constexpr auto kDefaultTimeout = std::chrono::milliseconds(5000);
constexpr auto kLongTimeout = std::chrono::milliseconds(30000);

}  // namespace

Ml307Hal::~Ml307Hal() {
  if (channel_) {
    UnregisterUrcHandlers();
  }
}

ModuleType Ml307Hal::GetModuleType() const {
  return ModuleType::kMl307;
}

std::string_view Ml307Hal::GetModuleName() const {
  return "ML307";
}

const ModuleCapabilities& Ml307Hal::GetCapabilities() const {
  static const ModuleCapabilities caps{
      .tcp = true,
      .udp = true,
      .ssl_tcp = true,
      .http = true,
      .https = true,
      .mqtt = true,
      .mqtts = true,
      .file_system = true,
      .low_power = true,
      .max_connections = kMaxConnections,
      .max_baud_rate = 921600,
  };
  return caps;
}

Result<> Ml307Hal::Initialize(IAtChannel& channel) {
  channel_ = &channel;

  // Whatever the caller had to do to get a reply out of the module - the wire
  // rate, in practice - has already been done: the channel handed here is one
  // the module answers on, because the caller could not have identified it
  // otherwise. See at_channel::FindModuleBaudRate.
  auto r = channel.SendCommand("ATE0", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  // Full functionality
  r = channel.SendCommand("AT+CFUN=1", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  // Per-socket settings (payload encoding, TLS) are deliberately not set here.
  // They are per-cid, and applying them at open time also repairs a cid that an
  // earlier run or a module reset left in a different state.
  RegisterUrcHandlers();

  return {};
}

Result<> Ml307Hal::Reset() {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  auto r = channel_->SendCommand("AT+CFUN=1,1", kLongTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

SimState Ml307Hal::GetSimState() {
  if (!channel_) return SimState::kUnknown;

  auto result = ReadSingleLineResponse("AT+CPIN?");
  if (!result) return SimState::kUnknown;

  auto response = result.value();
  if (response.find("READY") != std::string::npos) return SimState::kReady;
  if (response.find("SIM PIN") != std::string::npos) return SimState::kPinRequired;
  if (response.find("SIM PUK") != std::string::npos) return SimState::kPukRequired;
  if (response.find("NOT INSERTED") != std::string::npos)
    return SimState::kNotInserted;
  return SimState::kUnknown;
}

Result<> Ml307Hal::ConfigureApn(const ApnConfig& config) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }

  // Each step names itself in the error: a bare "CME error" from a three
  // command sequence says nothing about which one the module rejected.
  auto fail = [](const AtError& error, std::string_view step) {
    auto net = error.ToNetworkError();
    net.context = std::string(step) + ": " + net.context;
    return std::unexpected(net);
  };

  std::string cmd = "AT+CGDCONT=1,\"IP\",\"" + config.apn + "\"";
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) return fail(r.error(), "AT+CGDCONT");

  if (!config.username.empty()) {
    cmd = "AT+CGAUTH=1,1,\"" + config.username + "\",\"" +
          config.password + "\"";
    r = channel_->SendCommand(cmd, kDefaultTimeout);
    if (!r) return fail(r.error(), "AT+CGAUTH");
  }

  auto activate = ActivatePdp();
  if (!activate) {
    auto net = activate.error();
    net.context = "AT+MIPCALL: " + net.context;
    return std::unexpected(net);
  }
  return {};
}

RegistrationState Ml307Hal::GetRegistrationState() {
  if (!channel_) return RegistrationState::kUnknown;

  auto result = ReadSingleLineResponse("AT+CEREG?");
  if (!result) return RegistrationState::kUnknown;

  auto response = result.value();
  // Two shapes are possible depending on the configured report mode:
  //   +CEREG: <n>,<stat>   (n=0/2, the default)
  //   +CEREG: <stat>       (n=1)
  auto fields = SplitCsv(StripKeyPrefix(response));
  if (fields.empty()) return RegistrationState::kUnknown;

  auto stat_field = fields.size() >= 2 ? fields[1] : fields[0];
  auto stat = ParseInt(stat_field);
  if (!stat) return RegistrationState::kUnknown;

  switch (*stat) {
    case 0: return RegistrationState::kNotRegistered;
    case 1: return RegistrationState::kRegisteredHome;
    case 2: return RegistrationState::kSearching;
    case 3: return RegistrationState::kRegistrationDenied;
    case 4: return RegistrationState::kUnknown;
    case 5: return RegistrationState::kRegisteredRoaming;
    default: return RegistrationState::kUnknown;
  }
}

SignalInfo Ml307Hal::GetSignalInfo() {
  if (!channel_) return last_signal_;

  auto result = ReadSingleLineResponse("AT+CSQ");
  if (!result) return last_signal_;

  auto response = result.value();
  // +CSQ: <rssi>,<ber>
  auto fields = SplitCsv(StripKeyPrefix(response));
  if (fields.size() < 2) return last_signal_;

  SignalInfo info;
  auto rssi = ParseInt(fields[0]);
  auto ber = ParseInt(fields[1]);
  info.rssi = rssi ? *rssi : 99;
  info.ber = ber ? *ber : 99;
  last_signal_ = info;
  return info;
}

Result<std::string> Ml307Hal::GetImei() {
  // AT+CGSN on this firmware answers with the product serial ("20214M0030..."),
  // which is not an IMEI. AT+CGSN=1 returns the real 15-digit IMEI as
  // "+CGSN: <imei>". There is deliberately no fallback to AT+CGSN: handing
  // back a serial number where an IMEI was asked for is worse than failing.
  auto result = ReadSingleLineResponse("AT+CGSN=1");
  if (!result) return std::unexpected(result.error());
  return std::string(StripQuotes(StripKeyPrefix(result.value())));
}

Result<std::string> Ml307Hal::GetIccid() {
  // AT+CCID is not implemented on this firmware; it answers ERROR. The working
  // command is AT+ICCID, which replies "+ICCID: <iccid>".
  auto result = ReadSingleLineResponse("AT+ICCID");
  if (!result) return std::unexpected(result.error());
  return std::string(StripQuotes(StripKeyPrefix(result.value())));
}

Result<std::string> Ml307Hal::GetRevision() {
  return ReadSingleLineResponse("AT+CGMR");
}

Result<std::string> Ml307Hal::GetCarrier() {
  auto result = ReadSingleLineResponse("AT+COPS?");
  if (!result) return std::unexpected(result.error());

  auto response = result.value();
  // +COPS: <mode>,<format>,<oper>,<AcT>
  auto fields = SplitCsv(StripKeyPrefix(response));
  if (fields.size() < 3) {
    return std::unexpected(
        NetworkError(NetworkErrc::kProtocolError, 0, "failed to parse COPS"));
  }
  return std::string(StripQuotes(fields[2]));
}

Result<> Ml307Hal::SetFlightMode(bool enable) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  std::string cmd = enable ? "AT+CFUN=0" : "AT+CFUN=1";
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

bool Ml307Hal::SupportsLowPower() const {
  return true;
}

Result<> Ml307Hal::EnterSleep(const SleepConfig& config) {
  (void)config;
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  auto r = channel_->SendCommand("AT+SLEEP=2", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

Result<> Ml307Hal::ExitSleep() {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  auto r = channel_->SendCommand("AT+SLEEP=0", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

// === TCP ===

Result<int> Ml307Hal::TcpConnect(std::string_view host,
                                 uint16_t port,
                                 bool ssl) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  return OpenSocket("TCP", host, port, ssl);
}

Result<> Ml307Hal::TcpClose(int connect_id) {
  return CloseConnection(connect_id);
}

Result<int> Ml307Hal::TcpSend(int connect_id, const void* data, size_t len) {
  return SendHexPayload(connect_id, data, len);
}

// === UDP ===

Result<int> Ml307Hal::UdpOpen(std::string_view host, uint16_t port) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  // UDP is never TLS through this API, and the reference clears the ssl flag for
  // its udp sockets too, so each socket starts from a known state.
  return OpenSocket("UDP", host, port, false);
}

Result<> Ml307Hal::UdpClose(int connect_id) {
  return CloseConnection(connect_id);
}

Result<int> Ml307Hal::UdpSend(int connect_id, const void* data, size_t len) {
  return SendHexPayload(connect_id, data, len);
}

// === Built-in protocols ===

// Both return false until the firmware's own stacks are implemented. The module
// does have them - MIPHTTP and MIPMQTT - so this is a statement about what this
// HAL can currently construct, not about the hardware. It has to be false
// meanwhile: the capability is what sends an Auto-mode request down the builtin
// path, and a builtin path that cannot produce a client fails the request
// outright rather than letting it fall through to the software engine.
bool Ml307Hal::HasBuiltinHttp() const {
  return false;
}

bool Ml307Hal::HasBuiltinMqtt() const {
  return false;
}

// === Internal helpers ===

void Ml307Hal::RegisterUrcHandlers() {
  if (!channel_) return;

  urc_handles_.push_back(channel_->SubscribeUrc(
      "+MIPOPEN", [this](std::string_view cmd, std::string_view args) {
        OnMipopenUrc(cmd, args);
      }));

  urc_handles_.push_back(channel_->SubscribeUrc(
      "+MIPCLOSE", [this](std::string_view cmd, std::string_view args) {
        OnMipcloseUrc(cmd, args);
      }));

  urc_handles_.push_back(channel_->SubscribeUrc(
      "+MIPRTCP", [this](std::string_view cmd, std::string_view args) {
        OnMiprtcpUrc(cmd, args);
      }));

  urc_handles_.push_back(channel_->SubscribeUrc(
      "+MIPURC", [this](std::string_view cmd, std::string_view args) {
        OnMipUrc(cmd, args);
      }));

  urc_handles_.push_back(channel_->SubscribeUrc(
      "+CEREG", [this](std::string_view cmd, std::string_view args) {
        OnCeregUrc(cmd, args);
      }));
}

void Ml307Hal::UnregisterUrcHandlers() {
  if (!channel_) return;
  for (auto handle : urc_handles_) {
    channel_->UnsubscribeUrc(handle);
  }
  urc_handles_.clear();
}

int Ml307Hal::AllocateConnectionId(int first) {
  // Prefer a cid whose previous occupant has had time to finish announcing its
  // departure; see ConnectionSlot for why that matters.
  std::lock_guard<std::mutex> lock(pool_mutex_);

  int oldest = -1;
  std::chrono::steady_clock::time_point oldest_at{};

  for (int i = first; i < kMaxConnections; i++) {
    auto& slot = slots_[i];
    if (slot.used) continue;

    if (!slot.released ||
        std::chrono::steady_clock::now() - slot.released_at >= kCidQuarantine) {
      slot.used = true;
      return i;
    }

    if (oldest < 0 || slot.released_at < oldest_at) {
      oldest = i;
      oldest_at = slot.released_at;
    }
  }

  // Every free cid is still settling. Availability wins over a pristine cid:
  // refusing to connect would be worse than a small chance of inheriting a
  // teardown, and the oldest release is the least likely to still be talking.
  if (oldest >= 0) {
    slots_[oldest].used = true;
    return oldest;
  }
  return -1;
}

void Ml307Hal::ReleaseConnectionId(int id) {
  if (id < 0 || id >= kMaxConnections) return;

  // Held under the same lock as the allocation: the slot is written from the URC
  // thread and read from the caller's thread, and a reader that saw the "free"
  // flag without the release time would take a cid the quarantine exists to keep
  // out of circulation.
  std::lock_guard<std::mutex> lock(pool_mutex_);
  slots_[id].used = false;
  slots_[id].released = true;
  slots_[id].released_at = std::chrono::steady_clock::now();
}

bool Ml307Hal::IsConnectionUsed(int id) {
  if (id < 0 || id >= kMaxConnections) return false;
  std::lock_guard<std::mutex> lock(pool_mutex_);
  return slots_[id].used;
}

// === Socket setup helpers ===

// True when the module turned the open down because that cid is already taken.
//
// The module's view of which cids are in use outlives this process: a socket
// left open by an earlier run - or by any other program sharing the port -
// makes AT+MIPOPEN answer "+CME ERROR: 552" for that id even though the pool
// here believes it is free. Confirmed on ML307R-DL-MBRH0S01: the same command
// answers OK immediately after an AT+MIPCLOSE for that id.
//
// Only this synchronous rejection is retryable. A nonzero "+MIPOPEN: <id>,<code>"
// is a genuine connect failure - 753 is a TLS handshake the module could not
// negotiate - and retrying it would burn every remaining slot on a host that
// will never succeed.
bool IsConnectionIdBusy(const NetworkError& error) {
  constexpr int kCmeConnectionIdBusy = 552;
  return error.code == NetworkErrc::kAtCmeError &&
         error.native == kCmeConnectionIdBusy;
}

Result<int> Ml307Hal::OpenSocket(std::string_view type,
                                 std::string_view host,
                                 uint16_t port,
                                 bool ssl) {
  int first = 0;
  while (first < kMaxConnections) {
    int id = AllocateConnectionId(first);
    if (id < 0) break;

    auto release = [this, id](NetworkError error) {
      ReleaseConnectionId(id);
      return std::unexpected(std::move(error));
    };

    // A free slot in the pool above is not proof the cid is free on the module,
    // so ask before building on it. False means the cid was occupied and has only
    // just been closed: it is left to settle rather than reopened, because its
    // previous occupant's teardown events name the cid and nothing else - see
    // ConnectionSlot.
    auto ready = EnsureSocketFree(id);
    if (!ready) return release(ready.error());
    if (!*ready) {
      ReleaseConnectionId(id);
      first = id + 1;
      continue;
    }

    if (auto r = ConfigureSsl(id, ssl); !r) return release(r.error());
    if (auto r = ConfigureEncoding(id); !r) return release(r.error());

    if (auto opened = OpenOnId(id, type, host, port); !opened) {
      if (!IsConnectionIdBusy(opened.error())) {
        return release(opened.error());
      }
      // The cid went busy between the state check and the open. Hand it back and
      // resume the search past it, so the next attempt cannot land on the slot
      // the module just refused.
      ReleaseConnectionId(id);
      first = id + 1;
      continue;
    }
    return id;
  }

  if (first == 0) {
    return std::unexpected(NetworkError(
        NetworkErrc::kNoResources, 0, "no available connection slots"));
  }
  return std::unexpected(NetworkError(
      NetworkErrc::kNoResources, 0,
      "every connection slot is already in use on the module"));
}

Result<> Ml307Hal::OpenOnId(int id,
                            std::string_view type,
                            std::string_view host,
                            uint16_t port) {
  // The socket type is a quoted string and the timeout slot is left empty; the
  // positional form (MIPOPEN=<id>,0,...) answers "+CME ERROR: 50" on ML307R-DL
  // even though AT+MIPOPEN=? advertises a numeric field there.
  char cmd[256];
  std::snprintf(cmd, sizeof(cmd), "AT+MIPOPEN=%d,\"%.*s\",\"%.*s\",%u,,0", id,
                static_cast<int>(type.size()), type.data(),
                static_cast<int>(host.size()), host.data(),
                static_cast<unsigned>(port));

  ArmOpenWait(id);
  auto r = channel_->SendCommand(cmd, kLongTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  // The module does not always put its OK before the result: "+MIPOPEN: <id>,0"
  // was observed arriving inside the response buffer of a *later* command on
  // ML307R-DL-MBRH0S01. A line that arrives while a command is in flight is
  // absorbed into that command's buffer and never reaches the URC dispatcher
  // (AtUart::ProcessLine), so when it is this command's own result it is sitting
  // in these lines and the rendezvous below would wait out the full timeout for a
  // socket that is already open. Read it back out.
  for (auto line : channel_->GetResponseLines()) {
    if (line.rfind("+MIPOPEN", 0) != 0) continue;
    RecordOpenResult(StripKeyPrefix(line));
  }

  return WaitForOpenResult(id, kLongTimeout);
}

Result<bool> Ml307Hal::QuerySocketActive(int id) {
  char cmd[32];
  std::snprintf(cmd, sizeof(cmd), "AT+MIPSTATE=%d", id);

  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) {
    // A rejection is the module saying it does not answer this query, which is
    // the same position as an answer that carries no state: nothing is known, so
    // there is nothing to repair. Failing the connect here would mean a firmware
    // without the query could not open a socket at all. A transport failure is
    // different - the channel is broken and the caller has to hear about it.
    if (r.error().code == AtErrc::kCmeError) return false;
    return std::unexpected(r.error().ToNetworkError());
  }

  // +MIPSTATE: <id>,<type>,<host>,<port>,<state>, where a socket that was never
  // opened reports "INITIAL" with the description left empty:
  //   +MIPSTATE: 0,,,,"INITIAL"
  //   +MIPSTATE: 0,"TCP","www.baidu.com",80,"CONNECTED"
  //
  // The answer is a normal response line, not a URC, which is what makes it
  // usable as a synchronisation point - it cannot be absorbed into another
  // command's buffer the way an event can.
  //
  // It is also not necessarily the first line: a close event still in flight is
  // delivered ahead of it, observed on ML307R-DL-MBRH0S01 as
  //   +MIPCLOSE: 0 / +MIPSTATE: 0,,,,"INITIAL" / OK
  // so the line is looked for by name rather than taken positionally.
  for (auto line : channel_->GetResponseLines()) {
    if (line.rfind("+MIPSTATE", 0) != 0) continue;

    auto fields = SplitCsv(StripKeyPrefix(line));
    if (fields.size() < 5) continue;  // unrecognised shape; nothing to conclude
    return StripQuotes(fields[4]) != "INITIAL";
  }

  // No state line at all: the module has said nothing, so there is nothing to
  // repair. Reading silence as "held" would close a socket on every connect.
  return false;
}

Result<bool> Ml307Hal::EnsureSocketFree(int id) {
  auto active = QuerySocketActive(id);
  if (!active) return std::unexpected(active.error());

  // Already free: nothing of the previous occupant is outstanding, so this cid is
  // safe to open on. True means "ready", not merely "free".
  if (!*active) return true;

  char cmd[32];
  std::snprintf(cmd, sizeof(cmd), "AT+MIPCLOSE=%d", id);
  auto closed = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!closed && closed.error().code != AtErrc::kCmeError) {
    // A rejection means the module no longer considers the socket open, which is
    // the state being waited for. Anything else leaves its fate unknown.
    return std::unexpected(closed.error().ToNetworkError());
  }

  // AT+MIPCLOSE answers OK before the socket is gone. Wait for the module to
  // report the cid released so the close is at least under way - but see the
  // caller: a cid that had to be closed is not reused for this connect, because
  // the events announcing that close can still arrive afterwards.
  for (int attempt = 0; attempt < kClosePollAttempts; ++attempt) {
    std::this_thread::sleep_for(kClosePollInterval);

    auto state = QuerySocketActive(id);
    if (!state) return std::unexpected(state.error());
    if (!*state) return false;
  }

  // Still held. The caller skips the cid, which is the same outcome as a refusal.
  return false;
}

Result<> Ml307Hal::ConfigureSsl(int id, bool enable) {
  // TLS is not an AT+MIPOPEN argument on this module; it is a per-cid setting
  // applied beforehand. Enabling also requires the auth mode to be cleared
  // first, since the default asks for a client certificate we do not have.
  //
  // Note that "SSL" is not a socket type here - AT+MIPOPEN with it answers
  // "+CME ERROR: 50" - and the handshake happens during MIPOPEN, so a server
  // this firmware cannot negotiate with fails there with "+MIPOPEN: <id>,753"
  // rather than at the first send. The code is host-specific, not
  // configuration-specific: example.com returns 753 for every combination of
  // timeout, access mode and ordering tried against ML307R-DL-MBRH0S01, while
  // hosts with more conservative TLS settings answer 0. Raising 753 to the
  // caller as an opaque "connection failed" hides which of the two it was, so
  // the module's own code is preserved in `native`.
  if (enable) {
    auto auth = channel_->SendCommand("AT+MSSLCFG=\"auth\",0,0", kDefaultTimeout);
    if (!auth) return std::unexpected(auth.error().ToNetworkError());
  }

  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"ssl\",%d,%d,0", id,
                enable ? 1 : 0);
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

Result<> Ml307Hal::ConfigureEncoding(int id) {
  // Hex in both directions. Raw receive would corrupt any payload containing
  // \r\n, and raw send cannot carry a 0x1A byte without truncating the frame.
  // The two directions are configured independently as
  // "encoding",<cid>,<send>,<recv> and there is no wildcard cid, so this has to
  // run per socket.
  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"encoding\",%d,1,1", id);
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

void Ml307Hal::ArmOpenWait(int id) {
  std::lock_guard<std::mutex> lock(open_mutex_);
  open_wait_ = OpenWait{id, 0, false};
}

Result<> Ml307Hal::WaitForOpenResult(int id, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(open_mutex_);

  bool signalled =
      open_cv_.wait_for(lock, timeout, [this] { return open_wait_.done; });

  if (!signalled) {
    open_wait_ = OpenWait{};
    return std::unexpected(
        NetworkError(NetworkErrc::kTimeout, 0,
                     "timed out waiting for +MIPOPEN for connection " +
                         std::to_string(id)));
  }

  int code = open_wait_.code;
  open_wait_ = OpenWait{};

  if (code != 0) {
    return std::unexpected(NetworkError(
        NetworkErrc::kConnectFailed, code,
        "module refused connection " + std::to_string(id) + " (code " +
            std::to_string(code) + ")"));
  }
  return {};
}

Result<int> Ml307Hal::SendHexPayload(int connect_id,
                                     const void* data,
                                     size_t len) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }

  // The module takes the payload inline as hex: AT+MIPSEND=<id>,<len>,<hex>.
  // <len> counts payload bytes, not the doubled hex characters. Chunking keeps
  // the command line inside the module's input buffer; the reference uses
  // 1460/2 bytes for the same reason.
  constexpr size_t kMaxChunk = 1460 / 2;
  const auto* bytes = static_cast<const char*>(data);
  size_t total = 0;

  while (total < len) {
    size_t chunk = std::min(len - total, kMaxChunk);

    std::string cmd = "AT+MIPSEND=" + std::to_string(connect_id) + "," +
                      std::to_string(chunk) + ",";
    cmd += at_parser::EncodeHex(std::string_view(bytes + total, chunk));

    auto r = channel_->SendCommand(cmd, kLongTimeout);
    if (!r) return std::unexpected(r.error().ToNetworkError());

    total += chunk;
  }

  return static_cast<int>(len);
}

Result<> Ml307Hal::CloseConnection(int connect_id) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }

  // Closing a socket we no longer hold is a no-op, not a failure. The module
  // announces the peer's close with its own URC and has already been released
  // from the pool by the time a caller tidies up.
  if (!IsConnectionUsed(connect_id)) {
    return {};
  }

  char cmd[64];
  std::snprintf(cmd, sizeof(cmd), "AT+MIPCLOSE=%d", connect_id);
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  ReleaseConnectionId(connect_id);

  if (!r) {
    // An explicit rejection means the module does not consider the socket open,
    // which is exactly the state this call exists to reach: observed as
    // "+CME ERROR: 551" when closing after the peer had already gone. A timeout
    // or transmit failure is different - the socket's fate is genuinely unknown
    // - so only the rejection is folded into success.
    if (r.error().code == AtErrc::kCmeError) {
      return {};
    }
    return std::unexpected(r.error().ToNetworkError());
  }
  return {};
}

void Ml307Hal::OnMipopenUrc(std::string_view /*command*/,
                            std::string_view args) {
  RecordOpenResult(args);
}

void Ml307Hal::RecordOpenResult(std::string_view args) {
  // +MIPOPEN: <id>,<code>. Code 0 means the socket is up. Called both from the
  // URC handler and, when the line was absorbed into the command's own response
  // buffer instead of being dispatched, by the code that sent the command.
  auto fields = SplitCsv(args);
  if (fields.size() < 2) return;

  auto id = ParseInt(fields[0]);
  auto code = ParseInt(fields[1]);
  if (!id || !code) return;

  {
    std::lock_guard<std::mutex> lock(open_mutex_);
    if (open_wait_.id != *id) return;  // not the connection we are waiting on
    open_wait_.code = *code;
    open_wait_.done = true;
  }
  open_cv_.notify_all();
}

void Ml307Hal::OnMipcloseUrc(std::string_view /*command*/,
                             std::string_view args) {
  // +MIPCLOSE: <id>. The module dropped the socket.
  auto fields = SplitCsv(args);
  if (fields.empty()) return;

  auto id = ParseInt(fields[0]);
  if (!id) return;

  ReleaseConnectionId(*id);
  DispatchTcpClose(*id);
}

void Ml307Hal::OnMiprtcpUrc(std::string_view /*command*/,
                            std::string_view args) {
  // +MIPRTCP: <id>,<len>,<data_hex>
  auto fields = SplitCsv(args);
  if (fields.size() < 3) return;

  auto id = ParseInt(fields[0]);
  auto len = ParseInt(fields[1]);
  if (!id || !len) return;

  // Data is hex-encoded (recv_format=1)
  auto hex_data = StripQuotes(fields[2]);
  auto data = at_parser::ParseHex(hex_data);

  DispatchTcpData(*id,
                  std::string_view(reinterpret_cast<const char*>(data.data()),
                                   data.size()));
}

void Ml307Hal::OnMipUrc(std::string_view /*command*/, std::string_view args) {
  // +MIPURC: "<event>",<id>,...
  // Events seen on ML307R-DL: "rtcp" (inbound TCP payload, hex), "rudp"
  // (inbound UDP datagram, hex) and "disconn" (peer closed). The event name is
  // a quoted string, so it is field 0 while the connection id is field 1. The
  // two data events use separate names, so a socket type that only handled
  // "rtcp" would silently receive nothing on UDP.
  auto fields = SplitCsv(args);
  if (fields.size() < 2) return;

  auto event = StripQuotes(fields[0]);
  auto id = ParseInt(fields[1]);
  if (!id) return;

  if (event == "rtcp") {
    // +MIPURC: "rtcp",<id>,<len>,<hex data>
    if (fields.size() < 4) return;
    auto data = at_parser::ParseHex(StripQuotes(fields[3]));
    DispatchTcpData(*id, std::string_view(
                              reinterpret_cast<const char*>(data.data()),
                              data.size()));
  } else if (event == "rudp") {
    // +MIPURC: "rudp",<id>,<len>,<hex data>
    // The reference implementation for this module family reads exactly these
    // four fields and no more, so any source address a firmware variant appends
    // is treated as optional rather than assumed.
    if (fields.size() < 4) return;
    auto data = at_parser::ParseHex(StripQuotes(fields[3]));
    std::string_view host;
    uint16_t port = 0;
    if (fields.size() >= 6) {
      host = StripQuotes(fields[4]);
      if (auto parsed = ParseInt(fields[5])) {
        port = static_cast<uint16_t>(*parsed);
      }
    }
    DispatchUdpData(*id, host, port,
                    std::string_view(reinterpret_cast<const char*>(data.data()),
                                     data.size()));
  } else if (event == "disconn") {
    ReleaseConnectionId(*id);
    DispatchTcpClose(*id);
  }
}

void Ml307Hal::OnCeregUrc(std::string_view /*command*/,
                          std::string_view args) {
  // +CEREG: <stat>  or  +CEREG: <stat>[,...]
  auto fields = SplitCsv(args);
  if (fields.empty()) return;

  // The first field is either <n> (if format includes mode) or <stat>
  // ML307 default output (n=0): just +CEREG: <stat>
  // We use n=1: +CEREG: <stat>
  auto stat_idx = fields.size() >= 2 ? 1 : 0;
  auto stat = ParseInt(fields[stat_idx]);
  if (!stat) return;

  RegistrationState state;
  switch (*stat) {
    case 0: state = RegistrationState::kNotRegistered; break;
    case 1: state = RegistrationState::kRegisteredHome; break;
    case 2: state = RegistrationState::kSearching; break;
    case 3: state = RegistrationState::kRegistrationDenied; break;
    case 4: state = RegistrationState::kUnknown; break;
    case 5: state = RegistrationState::kRegisteredRoaming; break;
    default: state = RegistrationState::kUnknown; break;
  }

  registration_state_ = state;
  if (on_network_state_) {
    on_network_state_(state);
  }
}

Result<std::string> Ml307Hal::ReadSingleLineResponse(
    std::string_view cmd, std::chrono::milliseconds timeout) {
  auto r = channel_->SendCommand(cmd, timeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  auto lines = channel_->GetResponseLines();
  if (lines.empty()) {
    return std::unexpected(
        NetworkError(NetworkErrc::kProtocolError, 0, "empty response"));
  }

  return std::string(lines[0]);
}

Result<> Ml307Hal::ActivatePdp() {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }

  // The module usually brings a context up on its own at boot. Asking again in
  // that state answers "CME ERROR: 50" even though the data path is perfectly
  // healthy, so check first and make activation idempotent.
  auto query = ReadSingleLineResponse("AT+MIPCALL?");
  if (query) {
    // +MIPCALL: <flag>,<active>[,"<ipv4>","<ipv6>"]. Active is 0 or 1.
    auto fields = SplitCsv(StripKeyPrefix(query.value()));
    if (fields.size() >= 2) {
      auto active = ParseInt(fields[1]);
      if (active && *active == 1) return {};
    }
  }

  // AT+MIPCALL=? reports "(0-1),(1-15)": the context id is mandatory and must
  // be at least 1. A bare AT+MIPCALL=1, or a cid of 0, answers ERROR.
  auto r = channel_->SendCommand("AT+MIPCALL=1,1", kLongTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

// === Detection & factory ===

void ForceLinkMl307Hal() {
  // Empty function - referenced to ensure linker includes this translation unit
  // so the static ModuleRegistrar runs and registers ML307.
}

bool DetectMl307(IAtChannel& channel) {
  // First try basic AT
  auto r = channel.SendCommand("AT", std::chrono::milliseconds(1000));
  if (!r) return false;

  // Check firmware revision
  r = channel.SendCommand("AT+CGMR", std::chrono::milliseconds(1000));
  if (!r) return false;

  auto lines = channel.GetResponseLines();
  for (const auto& line : lines) {
    if (line.find("ML307") != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<hal::IModuleHal> CreateMl307Hal() {
  return std::make_unique<Ml307Hal>();
}

// Register with module registry
namespace {
struct Ml307Registrar {
  Ml307Registrar() {
    ::esp_modem_link::hal::ModuleRegistry::Instance().Register({
        ::esp_modem_link::ModuleType::kMl307,
        "ML307",
        DetectMl307,
        CreateMl307Hal,
    });
  }
};
static Ml307Registrar g_ml307_registrar;
}  // namespace

}  // namespace esp_modem_link::modules::ml307
