#include "modules/air780e/air780e_hal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "at_parser/at_parser.h"
#include "hal/module_registry.h"

namespace esp_modem_link::modules::air780e {

using at_channel::IAtChannel;
using at_parser::ParseInt;
using at_parser::SplitCsv;
using at_parser::StripKeyPrefix;
using at_parser::StripQuotes;

namespace {

// SplitCsv only splits - it does not trim, because a quoted field may legitimately
// hold a space. The module's event lines are written "<id>, CONNECT OK", so the
// separator brings a leading blank into field 1 and every field that is compared
// or parsed has to be trimmed first.
std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

// The first TlsConfig field this firmware cannot honour, or empty.
//
// This module encrypts but cannot authenticate: no command it accepts installs a
// certificate authority and none turns verification on. AT+SSLCFG takes writes
// and exposes no readable table, and the AT+CSSLCFG / AT+CCH* / AT+CCERT* family
// that would carry one answers ERROR on this firmware. So every one of these is a
// guarantee the caller asked for and would not get, and the socket that came up
// instead would be encrypted and silently unauthenticated - which is a weaker
// thing than what was requested, reported as success.
//
// handshake_timeout is deliberately not in the list: it is the one field the
// module does have a setting for, and it is applied by the connect wait rather
// than by a command.
std::string_view UnfulfillableTlsOption(const TlsConfig& config) {
  if (config.verify_certificate) return "verify_certificate";
  if (config.verify_hostname) return "verify_hostname";
  if (!config.ca_cert.empty()) return "ca_cert";
  if (!config.client_cert.empty()) return "client_cert";
  if (!config.client_key.empty()) return "client_key";
  if (!config.alpn_protocols.empty()) return "alpn_protocols";
  return {};
}

}  // namespace

Air780eHal::Air780eHal() : Air780eHal(Options{}) {}

Air780eHal::Air780eHal(Options options) : start_reader_(options.start_reader) {}

Air780eHal::~Air780eHal() {
  // The receive task is stopped before the handlers are withdrawn. It sends
  // commands, so a task still running against a channel whose handlers have just
  // been removed would be draining sockets nothing is listening to - and it holds
  // the channel pointer that is about to go away.
  StopReader();
  if (channel_) {
    UnregisterUrcHandlers();
  }
}

ModuleType Air780eHal::GetModuleType() const {
  return ModuleType::kAir780E;
}

std::string_view Air780eHal::GetModuleName() const {
  return "AIR780E";
}

const ModuleCapabilities& Air780eHal::GetCapabilities() const {
  // ssl_tcp is true because the module does terminate TLS, verified end to end
  // against port 443. Note what it does not promise: AT+CIPSSL is a single
  // module-wide switch, so a TLS socket and a plaintext one cannot be open at the
  // same time, and this firmware has no command to install a certificate
  // authority or to turn verification on - the AT+SSLCFG family that would do it
  // answers OK without exposing a readable table, and AT+CSSLCFG, AT+CCHOPEN and
  // AT+CCERT* do not exist here at all. So TlsConfig's verification fields cannot
  // be honoured, and TcpConnect refuses them rather than opening a socket that is
  // encrypted but silently unauthenticated when authentication was asked for.
  static const ModuleCapabilities caps{
      .tcp = true,
      .udp = true,
      .ssl_tcp = true,
      .http = true,
      .https = true,
      .mqtt = true,
      .mqtts = true,
      .file_system = false,
      .low_power = true,
      .max_connections = kMaxConnections,
      .max_baud_rate = 921600,
  };
  return caps;
}

Result<> Air780eHal::Initialize(IAtChannel& channel) {
  channel_ = &channel;

  // Handlers first: AT+CIPSHUT is answered with "SHUT OK", which is an event
  // rather than a response line, so there is nothing to read it from until the
  // subscription exists.
  RegisterUrcHandlers();

  // Whatever the caller had to do to get a reply out of the module - the wire
  // rate, in practice - has already been done: the channel handed here is one the
  // module answers on, because the caller could not have identified it otherwise.
  auto r = channel.SendCommand("ATE0", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  r = channel.SendCommand("AT+CFUN=1", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  // AT+CIPSHUT, not one AT+CIPCLOSE per cid. A cid whose connect failed or whose
  // peer closed sits in CLOSED and refuses to be closed - AT+CIPCLOSE answers
  // "+CME ERROR: 3" - and while one exists AT+CIPMUX rejects every change. Only
  // CIPSHUT resets them all to INITIAL, and it does so with sockets open and
  // without touching the CIPMUX setting. This is also what clears a socket left
  // behind by an earlier process.
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    shut_ok_ = false;
  }
  r = channel.SendLine("AT+CIPSHUT");
  if (!r) return std::unexpected(r.error().ToNetworkError());

  {
    std::unique_lock<std::mutex> lock(event_mutex_);
    event_cv_.wait_for(lock, kLongTimeout, [this] { return shut_ok_; });
    if (!shut_ok_) {
      return std::unexpected(NetworkError(
          NetworkErrc::kTimeout, 0, "AT+CIPSHUT was not confirmed with SHUT OK"));
    }
  }

  // Multi-connection mode, and it has to be read back rather than assumed: with a
  // cid still live the module answers "+CME ERROR: 3" and the mode silently stays
  // 0, in which case every AT+CIPSTART=<id>,... below is invalid and the HAL would
  // be parsing responses the module never sends.
  r = channel.SendCommand("AT+CIPMUX=1", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  auto mux = channel.SendCommand("AT+CIPMUX?", kDefaultTimeout);
  if (!mux) return std::unexpected(mux.error().ToNetworkError());

  {
    bool mux_on = false;
    for (const auto& line : channel.GetResponseLines()) {
      auto value = ParseInt(Trim(StripKeyPrefix(line)));
      if (value) mux_on = (*value == 1);
    }
    if (!mux_on) {
      return std::unexpected(NetworkError(
          NetworkErrc::kProtocolError, 0,
          "AT+CIPMUX? does not report 1, so the module is not in "
          "multi-connection mode and AT+CIPSTART=<id>,... cannot be used"));
    }
  }

  // Manual receive: the module announces arrivals with "+CIPRXGET: 1,<id>" and
  // carries nothing, so payload is fetched with AT+CIPRXGET=3 when the reader is
  // ready for it. 5 is "report on every arrival"; the alternative, 1, reports an
  // arrival only after the buffer has been read once.
  r = channel.SendCommand("AT+CIPRXGET=5", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  // Carry the sender's address on received UDP data. It is an appended field, so
  // the reply shapes with and without it differ in arity, and the parser has to
  // tolerate both - which it does by naming the fields it wants rather than
  // counting them.
  r = channel.SendCommand("AT+CIPSRIP=1", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  // Start from plaintext. Nothing is open yet, so this only brings the switch to
  // a known state; it is left alone afterwards until a socket needs it changed.
  {
    auto switched = ApplySslSwitch(false);
    if (!switched) return std::unexpected(switched.error());
  }

  {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    rx_stop_ = false;
    for (bool& pending : rx_pending_) pending = false;
  }
  if (start_reader_) {
    rx_task_ = platform::CreateTask(
        "air780e_rx", [this] { ReceiveLoop(); }, 4096, 5);
    rx_task_->Start();
  }

  return {};
}

Result<> Air780eHal::Reset() {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  auto r = channel_->SendCommand("AT+CFUN=1,1", kLongTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

// === SIM & Network ===

SimState Air780eHal::GetSimState() {
  if (!channel_) return SimState::kUnknown;

  auto result = ReadSingleLineResponse("AT+CPIN?");
  if (!result) return SimState::kUnknown;

  const auto& response = result.value();
  if (response.find("READY") != std::string::npos) return SimState::kReady;
  if (response.find("SIM PIN") != std::string::npos)
    return SimState::kPinRequired;
  if (response.find("SIM PUK") != std::string::npos)
    return SimState::kPukRequired;
  if (response.find("NOT INSERTED") != std::string::npos)
    return SimState::kNotInserted;
  return SimState::kUnknown;
}

Result<> Air780eHal::ConfigureApn(const ApnConfig& config) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }

  // Each step names itself in the error: a bare "CME error" out of a three
  // command sequence says nothing about which one the module rejected.
  auto fail = [](const AtError& error, std::string_view step) {
    auto net = error.ToNetworkError();
    net.context = std::string(step) + ": " + net.context;
    return std::unexpected(net);
  };

  // The APN is a PDP context definition (AT+CGDCONT) and then an activation
  // (AT+CGACT) - not ML307's AT+MIPCALL and not AT+CSTT, which on this firmware
  // reads back empty and is not the path the module uses.
  std::string cmd = "AT+CGDCONT=1,\"IP\",\"" + config.apn + "\"";
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) return fail(r.error(), "AT+CGDCONT");

  if (!config.username.empty()) {
    cmd = "AT+CGAUTH=1,1,\"" + config.username + "\",\"" + config.password +
          "\"";
    r = channel_->SendCommand(cmd, kDefaultTimeout);
    if (!r) return fail(r.error(), "AT+CGAUTH");
  }

  auto activate = ActivatePdp();
  if (!activate) {
    auto net = activate.error();
    net.context = "AT+CGACT: " + net.context;
    return std::unexpected(net);
  }
  return {};
}

Result<> Air780eHal::ActivatePdp() {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }

  // The module usually brings a context up on its own at boot, and asking again
  // in that state is rejected even though the data path is healthy - so read the
  // state first and make this idempotent.
  auto query = ReadSingleLineResponse("AT+CGACT?");
  if (query) {
    // +CGACT: <cid>,<state>, 1 meaning activated.
    auto fields = SplitCsv(StripKeyPrefix(query.value()));
    if (fields.size() >= 2) {
      auto state = ParseInt(Trim(fields[1]));
      if (state && *state == 1) return {};
    }
  }

  auto r = channel_->SendCommand("AT+CGACT=1,1", kLongTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

RegistrationState Air780eHal::GetRegistrationState() {
  if (!channel_) return RegistrationState::kUnknown;

  auto result = ReadSingleLineResponse("AT+CEREG?");
  if (!result) return RegistrationState::kUnknown;

  // The command is AT+CEREG? but this firmware answers with a "+CGREG:" key -
  // measured, and worth naming because a caller comparing the two would read it
  // as the wrong registration being reported. The field layout is the same, and
  // both shapes are accepted here in case another firmware answers +CEREG.
  auto fields = SplitCsv(StripKeyPrefix(result.value()));
  if (fields.empty()) return RegistrationState::kUnknown;

  auto stat = ParseInt(Trim(fields.size() >= 2 ? fields[1] : fields[0]));
  if (!stat) return RegistrationState::kUnknown;

  switch (*stat) {
    case 0: return RegistrationState::kNotRegistered;
    case 1: return RegistrationState::kRegisteredHome;
    case 2: return RegistrationState::kSearching;
    case 3: return RegistrationState::kRegistrationDenied;
    case 5: return RegistrationState::kRegisteredRoaming;
    default: return RegistrationState::kUnknown;
  }
}

SignalInfo Air780eHal::GetSignalInfo() {
  if (!channel_) return last_signal_;

  auto result = ReadSingleLineResponse("AT+CSQ");
  if (!result) return last_signal_;

  // +CSQ: <rssi>,<ber>
  auto fields = SplitCsv(StripKeyPrefix(result.value()));
  if (fields.size() < 2) return last_signal_;

  SignalInfo info;
  auto rssi = ParseInt(Trim(fields[0]));
  auto ber = ParseInt(Trim(fields[1]));
  info.rssi = rssi ? *rssi : 99;
  info.ber = ber ? *ber : 99;
  last_signal_ = info;
  return info;
}

// === Device info ===

Result<std::string> Air780eHal::GetImei() {
  // AT+CGSN returns the bare IMEI with no key and no quotes, and AT+CGSN=1
  // returns it as "+CGSN: "<imei>"". The keyed form is used because it is the one
  // that can be told apart from an error page or a stray line.
  auto result = ReadSingleLineResponse("AT+CGSN=1");
  if (!result) return std::unexpected(result.error());
  return std::string(StripQuotes(StripKeyPrefix(result.value())));
}

Result<std::string> Air780eHal::GetIccid() {
  // "+ICCID: <iccid>", unquoted on this firmware; StripQuotes also accepts a
  // quoted form so a firmware that quotes it is handled too.
  auto result = ReadSingleLineResponse("AT+ICCID");
  if (!result) return std::unexpected(result.error());
  return std::string(StripQuotes(StripKeyPrefix(result.value())));
}

Result<std::string> Air780eHal::GetRevision() {
  auto result = ReadSingleLineResponse("AT+CGMR");
  if (!result) return std::unexpected(result.error());
  // "+CGMR: "AirM2M_780EPV_V1004_LTE_AT"" -> the version string itself. Returned
  // without its key so that a caller comparing it against a version does not have
  // to know how the module frames it.
  return std::string(StripQuotes(StripKeyPrefix(result.value())));
}

Result<std::string> Air780eHal::GetCarrier() {
  auto result = ReadSingleLineResponse("AT+COPS?");
  if (!result) return std::unexpected(result.error());

  // +COPS: <mode>,<format>,"<oper>",<AcT>
  auto fields = SplitCsv(StripKeyPrefix(result.value()));
  if (fields.size() < 3) {
    return std::unexpected(
        NetworkError(NetworkErrc::kProtocolError, 0, "failed to parse COPS"));
  }
  return std::string(StripQuotes(Trim(fields[2])));
}

Result<> Air780eHal::SetFlightMode(bool enable) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  std::string cmd = enable ? "AT+CFUN=0" : "AT+CFUN=1";
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

bool Air780eHal::SupportsLowPower() const {
  return true;
}

Result<> Air780eHal::EnterSleep(const SleepConfig& config) {
  (void)config;
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  // AT+CSCLK takes 0-3, where 0 disables sleep and the rest select how the module
  // is woken. Level 2 is the DTR-driven one, which is what a caller asking for a
  // sleep it can leave on demand wants; a caller that disabled DTR wakeup gets
  // level 1, which is the same sleep without that path out.
  const char* cmd = config.enable_dtr_wakeup ? "AT+CSCLK=2" : "AT+CSCLK=1";
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

Result<> Air780eHal::ExitSleep() {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  auto r = channel_->SendCommand("AT+CSCLK=0", kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());
  return {};
}

// === TCP / UDP ===

Result<int> Air780eHal::TcpConnect(std::string_view host,
                                   uint16_t port,
                                   bool ssl,
                                   const TlsConfig& config) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }

  // Checked whatever ssl says. On a TLS socket these fields are guarantees that
  // would not be honoured; on a plaintext one they describe a handshake that is
  // not going to happen at all, and a caller who set them has asked for
  // something other than a plaintext socket.
  auto unfulfillable = UnfulfillableTlsOption(config);
  if (!unfulfillable.empty()) {
    return std::unexpected(NetworkError(
        NetworkErrc::kNotSupported, 0,
        "this firmware cannot honour TlsConfig." + std::string(unfulfillable) +
            ": no command it accepts installs a certificate authority or turns "
            "verification on, so a TLS socket here is encrypted but "
            "unauthenticated"));
  }

  return OpenSocket("TCP", host, port, ssl, config);
}

Result<> Air780eHal::TcpClose(int connect_id) {
  return CloseConnection(connect_id);
}

Result<int> Air780eHal::TcpSend(int connect_id, const void* data, size_t len) {
  return SendPayload(connect_id, data, len);
}

Result<int> Air780eHal::UdpOpen(std::string_view host, uint16_t port) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  // UDP is never TLS through this API, and the module's single SSL switch means a
  // UDP socket has to wait for TLS sockets to be gone just as a TCP one does.
  return OpenSocket("UDP", host, port, false, TlsConfig{});
}

Result<> Air780eHal::UdpClose(int connect_id) {
  return CloseConnection(connect_id);
}

Result<int> Air780eHal::UdpSend(int connect_id, const void* data, size_t len) {
  return SendPayload(connect_id, data, len);
}

bool Air780eHal::HasBuiltinHttp() const {
  // The module does have its own stacks, but this HAL cannot construct a client
  // on them. It has to say false meanwhile: the capability is what sends an
  // Auto-mode request down the builtin path, and a builtin path that cannot
  // produce a client fails the request outright instead of falling through to the
  // software engine.
  return false;
}

bool Air780eHal::HasBuiltinMqtt() const {
  return false;
}

// === cid pool ===

bool Air780eHal::IsConnectionUsed(int id) {
  if (id < 0 || id >= kMaxConnections) return false;
  std::lock_guard<std::mutex> lock(pool_mutex_);
  return slots_[id].used;
}

bool Air780eHal::OwnsSerial(int id, uint32_t serial) {
  if (id < 0 || id >= kMaxConnections || serial == 0) return false;
  std::lock_guard<std::mutex> lock(pool_mutex_);
  return slots_[id].used && slots_[id].serial == serial;
}

bool Air780eHal::PeerCloseNoted(int id) {
  if (id < 0 || id >= kMaxConnections) return false;
  std::lock_guard<std::mutex> lock(rx_mutex_);
  return rx_close_serial_[id] != 0;
}

int Air780eHal::AllocateConnectionId(const bool* avoid) {
  int chosen = -1;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (int i = 0; i < kMaxConnections; ++i) {
      if (slots_[i].used) continue;
      if (avoid != nullptr && avoid[i]) continue;
      slots_[i].used = true;
      slots_[i].udp = false;
      slots_[i].ssl = false;
      slots_[i].holds_mode = false;
      slots_[i].serial = next_serial_++;
      chosen = i;
      break;
    }
  }

  // The cid is changing hands, so nothing held for the generation that had it
  // before can be handed to the one taking it. Retiring a cid already voids its
  // hold, and this module's handlers only act on cids it holds, so no path here
  // reaches a hold with no retirement behind it - the line is the pool's half of
  // the invariant, which is what makes it a property of taking a cid rather than
  // something each handler has to remember.
  if (chosen >= 0) DiscardUndelivered(chosen);
  return chosen;
}

std::optional<bool> Air780eHal::RetireSlot(int id) {
  if (id < 0 || id >= kMaxConnections) return std::nullopt;

  bool held_mode = false;
  bool owned = false;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (!slots_[id].used) return std::nullopt;
    owned = slots_[id].holds_mode;
    held_mode = slots_[id].ssl;
    slots_[id] = SocketSlot{};
  }

  // The cid is free the moment this returns. There is deliberately no quarantine:
  // all three ways a cid becomes available again - a clean close, a refused
  // connect, and the peer closing - were measured to accept an immediate
  // AT+CIPSTART, so holding one out of the rotation would cost a socket and buy
  // nothing.
  {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    rx_pending_[id] = false;
    // A close that was still waiting to be delivered belonged to the socket being
    // retired here, and the application is the one retiring it - so there is
    // nothing left for it to say. Clearing it is also what keeps it from being
    // inherited by whatever takes this cid next.
    rx_close_serial_[id] = 0;
    rx_peer_[id].clear();
  }
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_wait_[id] = SendWait{};
  }

  // Payload the module handed over for this cid while no route was registered for
  // it goes with it, for the same reason and with more force: a cid cannot tell
  // two of its generations apart, so a hold that outlived one would be handed to
  // the next client on this cid as if the previous socket's bytes were its own.
  DiscardUndelivered(id);

  if (!owned) return std::nullopt;
  return held_mode;
}

void Air780eHal::ReleaseConnectionId(int id) {
  if (auto owned_mode = RetireSlot(id)) {
    ReleaseSocketForMode(*owned_mode);
  }
}

// === opening ===

Result<int> Air780eHal::OpenSocket(std::string_view type,
                                   std::string_view host,
                                   uint16_t port,
                                   bool ssl,
                                   const TlsConfig& config) {
  (void)config;

  auto reserved = ReserveSocketForMode(ssl);
  if (!reserved) return std::unexpected(reserved.error());

  // Taken once for the call and given back on every path out of it. The search
  // below can allocate and let go of several slots, and none of them owns the
  // reservation until one is returned to the caller - which is what makes
  // retiring that socket the thing that releases it.

  // Cids already tried and found held by somebody else. A cid goes back to the
  // front of the queue the instant it is released, so without this the search
  // would be handed the same one every time and a module with a single foreign
  // socket would report as a module with none free.
  bool tried[kMaxConnections] = {};

  // A cid the module still holds is refused at the moment of use rather than
  // predicted beforehand: AT+CIPSTART answers "+CME ERROR: 65535" and then
  // "<id>, ALREADY CONNECT", which is a direct statement about that cid. Neither
  // query that suggested itself as a pre-flight check works - AT+CIPSTATUS sends
  // its table after its OK, so a synchronous reader never sees it, and
  // AT+CIPRXGET=4 reports a closed cid and an empty open socket identically.
  for (int attempt = 0; attempt < kMaxConnections; ++attempt) {
    int id = AllocateConnectionId(tried);
    if (id < 0) {
      ReleaseSocketForMode(ssl);
      return std::unexpected(NetworkError(
          NetworkErrc::kNoResources, 0,
          "all " + std::to_string(kMaxConnections) + " connection ids in use"));
    }

    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      slots_[id].udp = (type == "UDP");
    }

    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "AT+CIPSTART=%d,\"%.*s\",\"%.*s\",%u", id,
                  static_cast<int>(type.size()), type.data(),
                  static_cast<int>(host.size()), host.data(),
                  static_cast<unsigned>(port));

    ArmOpenWait(id);
    auto sent = channel_->SendLine(cmd);
    if (!sent) {
      RetireSlot(id);
      ReleaseSocketForMode(ssl);
      return std::unexpected(sent.error().ToNetworkError());
    }

    auto opened = WaitForOpenResult(id, kConnectTimeout);
    if (opened) {
      // The outcome is the module's word, so the pool learns nothing more here -
      // the handler already released or kept the cid. The reservation passes to
      // the slot at the same moment the socket passes to the caller.
      {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        slots_[id].ssl = ssl;
        slots_[id].holds_mode = true;
      }
      return id;
    }

    if (opened.error().native == kAlreadyConnectedCode) {
      // Held by somebody else, most likely a previous run of this program. It is
      // not ours to close on the way past, so it goes back as it was and the
      // search moves to another cid rather than destroying a socket this process
      // does not own.
      tried[id] = true;
      RetireSlot(id);
      continue;
    }

    // Any other failure leaves the cid reusable immediately, so it goes back.
    RetireSlot(id);
    ReleaseSocketForMode(ssl);
    return std::unexpected(opened.error());
  }

  ReleaseSocketForMode(ssl);
  return std::unexpected(NetworkError(
      NetworkErrc::kResourceBusy, 0,
      "every connection id is held by a socket this process did not open"));
}

Result<> Air780eHal::CloseConnection(int connect_id) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  if (connect_id < 0 || connect_id >= kMaxConnections) {
    return std::unexpected(NetworkError(NetworkErrc::kInvalidArgument, 0,
                                        "connection id out of range"));
  }

  // Closing a socket this process no longer holds is a no-op, not a failure. The
  // module announces a peer's close by itself and the cid has already been
  // released by the time a caller tidies up.
  if (!IsConnectionUsed(connect_id)) return {};

  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    close_ok_ = false;
    close_pending_id_ = connect_id;
  }

  char cmd[32];
  std::snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", connect_id);
  auto sent = channel_->SendLine(cmd);

  // The cid is released whatever the module says. A rejection here means the
  // module does not consider the socket open, which is the state this call exists
  // to reach - observed as "+CME ERROR: 3" both for a cid left in CLOSED by a
  // failed connect and for one whose peer has already gone.
  ReleaseConnectionId(connect_id);

  if (!sent) {
    // A transmit failure is different: the command never left, so the module may
    // well still hold the socket. Its fate is genuinely unknown.
    return std::unexpected(sent.error().ToNetworkError());
  }

  std::unique_lock<std::mutex> lock(event_mutex_);
  event_cv_.wait_for(lock, kDefaultTimeout, [this] { return close_ok_; });
  close_pending_id_ = -1;
  return {};
}

// === TLS ===

Result<> Air780eHal::ReserveSocketForMode(bool ssl) {
  {
    std::unique_lock<std::mutex> lock(ssl_mutex_);

    // AT+CIPSSL is deliberately sent with this mutex dropped; see the member
    // comment. Waiting here also means an opener that arrives mid-switch sees
    // the finished result rather than the state before it.
    ssl_cv_.wait(lock, [this] { return !ssl_switch_in_progress_; });

    const int opposite = ssl ? plain_sockets_open_ : ssl_sockets_open_;
    if (opposite > 0) {
      // Refused rather than queued. AT+CIPSSL=1 makes a connect to a plaintext
      // port answer CONNECT FAIL and AT+CIPSSL=0 makes a TLS port do the same,
      // so the two kinds genuinely cannot be open at once - and the caller gets
      // to hear that rather than watch a socket fail for a reason that looks
      // like the network.
      return std::unexpected(NetworkError(
          NetworkErrc::kResourceBusy, 0,
          ssl ? "AT+CIPSSL is one switch for the whole module, so a TLS socket "
                "cannot be opened while a plaintext socket is open"
              : "AT+CIPSSL is one switch for the whole module, so a plaintext "
                "socket cannot be opened while a TLS socket is open"));
    }

    if ((ssl ? ssl_sockets_open_ : plain_sockets_open_) > 0) {
      // Already open in this mode, so the switch is where it needs to be.
      ++(ssl ? ssl_sockets_open_ : plain_sockets_open_);
      return {};
    }

    // Nothing open either way, so the switch has to move - and it is marked in
    // flight before the mutex is dropped, so a second opener waits for this
    // switch instead of starting another one or slipping a CIPSTART in ahead.
    ssl_switch_in_progress_ = true;
  }

  auto r = ApplySslSwitch(ssl);

  {
    std::lock_guard<std::mutex> lock(ssl_mutex_);
    ssl_switch_in_progress_ = false;
    if (r) ++(ssl ? ssl_sockets_open_ : plain_sockets_open_);
  }
  ssl_cv_.notify_all();
  if (!r) return std::unexpected(r.error());
  return {};
}

void Air780eHal::ReleaseSocketForMode(bool ssl) {
  std::lock_guard<std::mutex> lock(ssl_mutex_);
  int& count = ssl ? ssl_sockets_open_ : plain_sockets_open_;
  if (count > 0) --count;
}

Result<> Air780eHal::ApplySslSwitch(bool enable) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }

  if (enable) {
    // The protocol version, which the switch alone does not select. Measured:
    // AT+CIPSSL=1 on its own leaves every handshake failing with CONNECT FAIL,
    // and the same connect succeeds once this has been set. 4 is TLS 1.2, and
    // the module advertises 0-4 where 0 is "not set".
    auto version =
        channel_->SendCommand("AT+SSLCFG=\"sslversion\",0,4", kDefaultTimeout);
    if (!version) return std::unexpected(version.error().ToNetworkError());
  }

  const char* cmd = enable ? "AT+CIPSSL=1" : "AT+CIPSSL=0";
  auto r = channel_->SendCommand(cmd, kDefaultTimeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  // Read back rather than trusting the OK. A switch that did not take would not
  // fail here - it would surface as CONNECT FAIL on every TLS socket from then
  // on, which reads as a network problem rather than as this setting.
  auto query = channel_->SendCommand("AT+CIPSSL?", kDefaultTimeout);
  if (!query) return std::unexpected(query.error().ToNetworkError());

  for (const auto& line : channel_->GetResponseLines()) {
    auto value = ParseInt(Trim(StripKeyPrefix(line)));
    if (value && (*value == 1) == enable) return {};
  }
  return std::unexpected(NetworkError(
      NetworkErrc::kProtocolError, 0,
      std::string("AT+CIPSSL? does not report ") + (enable ? "1" : "0") +
          " after AT+CIPSSL=" + (enable ? "1" : "0")));
}

void Air780eHal::ArmOpenWait(int id) {
  std::lock_guard<std::mutex> lock(open_mutex_);
  open_wait_ = OpenWait{id, OpenOutcome::kFailed, false};
}

Result<> Air780eHal::WaitForOpenResult(int id,
                                       std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(open_mutex_);
  const bool signalled =
      open_cv_.wait_for(lock, timeout, [this] { return open_wait_.done; });

  if (!signalled) {
    open_wait_ = OpenWait{};
    // Silence is a real answer from this module rather than a lost line: an
    // address that is routable but unreachable gets its OK and then no outcome at
    // all, indefinitely. So this timeout is the only thing that ends that case.
    return std::unexpected(NetworkError(
        NetworkErrc::kTimeout, 0,
        "no CONNECT OK, CONNECT FAIL or ALREADY CONNECT for connection " +
            std::to_string(id)));
  }

  const OpenOutcome outcome = open_wait_.outcome;
  open_wait_ = OpenWait{};

  switch (outcome) {
    case OpenOutcome::kConnected:
      return {};
    case OpenOutcome::kAlreadyConnected:
      return std::unexpected(NetworkError(
          NetworkErrc::kAlreadyConnected, kAlreadyConnectedCode,
          "the module still holds connection " + std::to_string(id)));
    case OpenOutcome::kFailed:
      break;
  }
  return std::unexpected(NetworkError(
      NetworkErrc::kConnectFailed, 0,
      "the module refused connection " + std::to_string(id)));
}

// === sending ===

Result<int> Air780eHal::SendPayload(int connect_id,
                                    const void* data,
                                    size_t len) {
  if (!channel_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotInitialized, 0, "channel not set"));
  }
  if (connect_id < 0 || connect_id >= kMaxConnections) {
    return std::unexpected(NetworkError(NetworkErrc::kInvalidArgument, 0,
                                        "connection id out of range"));
  }

  // Counted before the command goes out, so an acknowledgement that arrives while
  // AT+CIPSEND is still in flight cannot be missed.
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_wait_[connect_id].pending++;
    send_wait_[connect_id].failed = false;
  }

  char cmd[48];
  std::snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%zu", connect_id, len);

  // The acknowledgement is not awaited inside the channel. It names only the cid,
  // so only this side - which knows which chunk is outstanding on which cid - can
  // tell a late one from this one's.
  auto r = channel_->SendDataAfterPrompt(cmd, data, len, kLongTimeout);
  if (!r) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (send_wait_[connect_id].pending > 0) send_wait_[connect_id].pending--;
    // A socket the peer has already let go of answers the length line with
    // "+CME ERROR: 3" and never shows the prompt. Reported as the AT error it
    // literally is, "the server hung up while I was uploading" would reach the
    // caller looking exactly like "the command was malformed" - and a caller that
    // can tell those apart, which the HTTP client can, has no way to act on it.
    if (PeerCloseNoted(connect_id)) {
      return std::unexpected(NetworkError(
          NetworkErrc::kConnectionLost, 0,
          "the peer closed the connection before this send"));
    }
    return std::unexpected(r.error().ToNetworkError());
  }

  std::unique_lock<std::mutex> lock(send_mutex_);
  const bool acked = send_cv_.wait_for(lock, kDefaultTimeout, [&] {
    return send_wait_[connect_id].pending == 0 ||
           send_wait_[connect_id].failed;
  });
  const bool failed = send_wait_[connect_id].failed;

  // Given up on either way. A timed-out acknowledgement is abandoned rather than
  // left outstanding: it would otherwise be claimed by the next chunk, which is
  // the one thing the count exists to prevent.
  send_wait_[connect_id] = SendWait{};
  lock.unlock();

  if (failed) {
    return std::unexpected(NetworkError(
        NetworkErrc::kConnectFailed, 0,
        "AT+CIPSEND was refused on connection " + std::to_string(connect_id)));
  }
  if (!acked) {
    return std::unexpected(NetworkError(
        NetworkErrc::kTimeout, 0,
        "no SEND OK for connection " + std::to_string(connect_id)));
  }
  return static_cast<int>(len);
}

// === receive ===

void Air780eHal::ReceiveLoop() {
  while (true) {
    std::vector<int> pending;
    {
      std::unique_lock<std::mutex> lock(rx_mutex_);
      rx_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
        if (rx_stop_) return true;
        for (bool waiting : rx_pending_) {
          if (waiting) return true;
        }
        return false;
      });
      if (rx_stop_) return;

      // Taken and cleared under the lock, before any reading happens. An arrival
      // that lands while this pass is pulling is what re-arms the flag, so a
      // clear afterwards would drop exactly the notification that says more data
      // came in behind the first batch.
      for (int i = 0; i < kMaxConnections; ++i) {
        if (rx_pending_[i]) {
          rx_pending_[i] = false;
          pending.push_back(i);
        }
      }
    }

    for (int id : pending) {
      if (rx_stop_) return;
      if (!DrainOneCid(id)) continue;

      // The pass stopped at kMaxDrainPerWake with bytes still queued. AT+CIPRXGET=5
      // announces an arrival once and never repeats it, so without putting the flag
      // back the remainder would sit in the module's buffer with nothing left to
      // wake this task - the socket would simply look stalled, and a client reading
      // a response larger than one pass would time out and report a truncated body.
      // Re-arming costs nothing when the data really is done: the next pass reads a
      // length of zero and stops.
      std::lock_guard<std::mutex> lock(rx_mutex_);
      rx_pending_[id] = true;
    }
  }
}

void Air780eHal::DrainPending() {
  // Repeated until no cid reports more, so that a caller sees the same delivery a
  // reader that had been allowed to keep waking would have produced.
  while (true) {
    std::vector<int> pending;
    {
      std::lock_guard<std::mutex> lock(rx_mutex_);
      for (int i = 0; i < kMaxConnections; ++i) {
        if (rx_pending_[i]) {
          rx_pending_[i] = false;
          pending.push_back(i);
        }
      }
    }
    if (pending.empty()) return;

    for (int id : pending) {
      if (!DrainOneCid(id)) continue;
      std::lock_guard<std::mutex> lock(rx_mutex_);
      rx_pending_[id] = true;
    }
  }
}

bool Air780eHal::DrainOneCid(int id) {
  if (!channel_) return false;
  size_t drained = 0;

  while (drained < kMaxDrainPerWake) {
    // Re-checked every round: the cid can be released while this pass is
    // running, and a released cid goes straight back into the rotation.
    if (!IsConnectionUsed(id)) {
      // Whoever released it has already ended the route, so a close that was
      // still waiting for this socket has nothing left to say.
      std::lock_guard<std::mutex> lock(rx_mutex_);
      rx_close_serial_[id] = 0;
      return false;
    }

    bool udp = false;
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      udp = slots_[id].udp;
    }

    char cmd[48];
    std::snprintf(cmd, sizeof(cmd), "AT+CIPRXGET=3,%d,%d", id, kReadChunk);

    auto r = channel_->SendCommand(cmd, kDefaultTimeout);
    if (!r) {
      // "+CME ERROR: 3" is what a cid that has been closed answers. If the peer's
      // close has already been reported then this is its other half - the socket
      // has to be ended and the route closed, and nothing will come of reading
      // again - so that is done here. If no close was reported, this is a cid that
      // went away under us and the CLOSED line, when it comes, will be its own
      // event.
      FinishClosedCid(id);
      return false;
    }

    auto lines = channel_->GetResponseLines();

    // The reply is a header naming the length and then the payload as hex. An
    // empty socket answers a header of length 0 with no payload line at all,
    // which is also how a pass that has caught up reports itself - so the loop
    // ends on the length, and never needs a preceding count query to know whether
    // it is done.
    int returned = -1;
    std::string_view hex;
    std::string_view source;
    for (size_t i = 0; i < lines.size(); ++i) {
      auto fields = SplitCsv(StripKeyPrefix(lines[i]));
      if (fields.size() < 3) continue;

      auto which = ParseInt(Trim(fields[0]));
      auto found = ParseInt(Trim(fields[1]));
      if (!which || *which != 3 || !found || *found != id) continue;

      auto length = ParseInt(Trim(fields[2]));
      if (!length) continue;

      returned = *length;
      if (i + 1 < lines.size()) hex = lines[i + 1];
      // With AT+CIPSRIP=1 the sender's address is appended, which makes this the
      // one field whose presence varies by socket type and setting.
      if (fields.size() >= 5) source = Trim(fields[4]);
      break;
    }

    if (returned <= 0) {
      // Caught up. If the peer has already been reported gone, the socket is now
      // finished in both senses and this is where the route ends: after the last
      // byte rather than in front of it.
      FinishClosedCid(id);
      return false;
    }

    auto bytes = at_parser::ParseHex(hex);
    if (bytes.empty()) {
      // A header promising bytes with nothing decodable behind it. Ending the
      // pass is right: another read would fetch the same header again.
      FinishClosedCid(id);
      return false;
    }

    auto payload = std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());

    if (udp) {
      std::string_view peer = source;
      if (peer.empty()) {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        peer = rx_peer_[id];
      }
      auto colon = peer.rfind(':');
      if (colon == std::string_view::npos) {
        // Without an address there is nowhere to attribute the datagram to, and
        // handing it up with a made-up one would be worse than dropping it.
        return false;
      }
      auto port = ParseInt(peer.substr(colon + 1));
      DispatchUdpData(id, peer.substr(0, colon),
                      port ? static_cast<uint16_t>(*port) : 0, payload);
    } else {
      DispatchTcpData(id, payload);
    }

    drained += bytes.size();
  }

  // Reached only by running out of budget rather than by running out of data,
  // which is the one outcome the caller has to act on. See ReceiveLoop.
  return true;
}

void Air780eHal::FinishClosedCid(int id) {
  uint32_t serial = 0;
  {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    serial = rx_close_serial_[id];
    rx_close_serial_[id] = 0;
  }
  if (serial == 0) return;  // no peer close was reported for this socket

  // The serial is what keeps this from tearing down the wrong connection. A cid
  // is reusable the instant it is released, and the close is dispatched after the
  // read that exhausted the socket rather than at the moment it arrived - so by
  // now the application may have let this cid go and a new socket may be sitting
  // on the same number. Ending that one's route would be a different bug in place
  // of this one, and it would look like a random disconnect on a healthy socket.
  if (!OwnsSerial(id, serial)) return;

  ReleaseConnectionId(id);
  DispatchTcpClose(id);
}

void Air780eHal::StopReader() {
  {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    rx_stop_ = true;
  }
  rx_cv_.notify_all();
  if (rx_task_) {
    rx_task_->Stop();  // joins
    rx_task_.reset();
  }
}

// === URC handlers ===

void Air780eHal::RegisterUrcHandlers() {
  if (!channel_) return;

  urc_handles_.push_back(channel_->SubscribeUrc(
      "+CIPRXGET", [this](std::string_view, std::string_view args) {
        OnCiprxgetUrc(args);
      }));

  // The rest of this module's events are unsolicited lines with no key at all -
  // "<id>, CONNECT OK" and its siblings - so there is no prefix to subscribe to.
  // An empty prefix matches every line, which is why the handler matches the
  // line's shape rather than trusting that it was sent what it expected: it also
  // sees every response line that AtUart dispatches while a command is in flight,
  // including the module's own replies to the commands this HAL sends.
  urc_handles_.push_back(channel_->SubscribeUrc(
      "", [this](std::string_view command, std::string_view) {
        OnUnsolicitedLine(command);
      }));
}

void Air780eHal::UnregisterUrcHandlers() {
  if (!channel_) return;
  for (auto handle : urc_handles_) {
    channel_->UnsubscribeUrc(handle);
  }
  urc_handles_.clear();
}

void Air780eHal::OnCiprxgetUrc(std::string_view args) {
  // Only the arrival notification is of interest: "+CIPRXGET: 1,<id>" with an
  // optional quoted sender. The sibling "+CIPRXGET: 3,..." is this HAL's own read
  // reply, dispatched here by AtUart while the read command is in flight, and
  // treating it as an arrival would queue the reader against data it has already
  // taken.
  auto fields = SplitCsv(args);
  if (fields.size() < 2) return;

  auto kind = ParseInt(Trim(fields[0]));
  auto id = ParseInt(Trim(fields[1]));
  if (!kind || *kind != 1 || !id || *id < 0 || *id >= kMaxConnections) return;

  {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    // Kept as well as the flag: a UDP datagram is attributed to the sender that
    // carried it, and the arrival is where that address first appears.
    if (fields.size() >= 3) rx_peer_[*id] = std::string(Trim(fields[2]));
    rx_pending_[*id] = true;
  }
  rx_cv_.notify_all();
}

void Air780eHal::OnUnsolicitedLine(std::string_view line) {
  // A keyed line belongs to somebody else: the "+CIPRXGET" replies go to their own
  // handler, and "OK", "+CME ERROR:" and the rest are response lines that AtUart
  // dispatched here because they arrived while a command was open. Everything this
  // handler wants is a keyless event, so the colon is the whole filter.
  if (line.find(':') != std::string_view::npos) return;

  if (line == "SHUT OK") {
    {
      std::lock_guard<std::mutex> lock(event_mutex_);
      shut_ok_ = true;
    }
    event_cv_.notify_all();
    return;
  }

  // Everything below names its cid first, so the comma is what separates an event
  // this handler knows from a hex payload line or a version string.
  const auto comma = line.find(',');
  if (comma == std::string_view::npos) return;

  auto id = ParseInt(Trim(line.substr(0, comma)));
  if (!id || *id < 0 || *id >= kMaxConnections) return;

  const std::string_view what = Trim(line.substr(comma + 1));

  if (what == "CONNECT OK") {
    RecordOpenResult(*id, OpenOutcome::kConnected);
  } else if (what == "CONNECT FAIL") {
    RecordOpenResult(*id, OpenOutcome::kFailed);
  } else if (what == "ALREADY CONNECT") {
    RecordOpenResult(*id, OpenOutcome::kAlreadyConnected);
  } else if (what == "CLOSE OK") {
    bool ours = false;
    {
      std::lock_guard<std::mutex> lock(event_mutex_);
      if (close_pending_id_ == *id) {
        close_ok_ = true;
        ours = true;
      }
    }
    if (ours) {
      event_cv_.notify_all();
    } else {
      // Not an answer to a close this process asked for, so it is the module
      // announcing that the peer is gone - the same event as "<id>, CLOSED" and
      // the one this firmware actually sends on a TLS socket. Measured, with
      // nothing but back-to-back reads on the wire: the last read left 2099 bytes,
      // this line arrived, and every read after it answered "+CME ERROR: 3" with
      // the buffer gone. Ignoring it because no close was pending leaves the
      // client waiting on a socket that is already finished.
      NotePeerClosed(*id);
    }
  } else if (what == "SEND OK") {
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      if (send_wait_[*id].pending > 0) send_wait_[*id].pending--;
    }
    send_cv_.notify_all();
  } else if (what == "SEND FAIL") {
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      send_wait_[*id].failed = true;
      send_wait_[*id].pending = 0;
    }
    send_cv_.notify_all();
  } else if (what == "CLOSED") {
    NotePeerClosed(*id);
  }
}

void Air780eHal::NotePeerClosed(int id) {
  // The module reports a peer's close under two different lines - "<id>, CLOSED"
  // and a "<id>, CLOSE OK" nobody asked for - and which one arrives depends on the
  // socket rather than on anything the caller did. Both mean the same thing, so
  // both come here.
  //
  // It is not, however, the end of the data. Measured: a read of 720 bytes came
  // back for this cid *after* the close line, so the module can report the close
  // while bytes it has already taken in are still queued for us. Acting on it here
  // would end the route at once, and everything behind the close would be handed
  // to nobody - which is how a page arrives as "connection closed before the
  // response was complete" with the last chunk missing rather than the body
  // stopping where the page does.
  //
  // So the close is recorded against the socket it is about and the reader is
  // woken to finish the socket first. Draining cannot be done here: this runs on
  // the AT receive task, and every read is a command whose reply only that same
  // task can deliver, so a read issued from a handler would wait on itself.
  uint32_t serial = 0;
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (id >= 0 && id < kMaxConnections && slots_[id].used) {
      serial = slots_[id].serial;
    }
  }
  if (serial == 0) return;

  {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    rx_close_serial_[id] = serial;
    rx_pending_[id] = true;
  }
  rx_cv_.notify_all();
}

void Air780eHal::RecordOpenResult(int id, OpenOutcome outcome) {
  {
    std::lock_guard<std::mutex> lock(open_mutex_);
    if (open_wait_.id != id) return;  // not the connection being waited on
    open_wait_.outcome = outcome;
    open_wait_.done = true;
  }
  open_cv_.notify_all();
}

// === identity and network helpers ===

Result<std::string> Air780eHal::ReadSingleLineResponse(
    std::string_view cmd, std::chrono::milliseconds timeout) {
  auto r = channel_->SendCommand(cmd, timeout);
  if (!r) return std::unexpected(r.error().ToNetworkError());

  auto lines = channel_->GetResponseLines();
  if (lines.empty()) {
    return std::unexpected(
        NetworkError(NetworkErrc::kProtocolError, 0, "empty response"));
  }
  return lines[0];
}

// === detection & factory ===

void ForceLinkAir780eHal() {
  // Empty function - referenced to ensure the linker includes this translation
  // unit so the static ModuleRegistrar runs and registers AIR780E.
}

bool DetectAir780e(IAtChannel& channel) {
  auto r = channel.SendCommand("AT", std::chrono::milliseconds(1000));
  if (!r) return false;

  r = channel.SendCommand("AT+CGMR", std::chrono::milliseconds(1000));
  if (!r) return false;

  // This firmware answers "AirM2M_780EPV_V1004_LTE_AT". The match is on the
  // vendor prefix rather than the model, because the prefix is what the family
  // shares and a probe that insisted on the model would reject a sibling that
  // speaks the same command set.
  for (const auto& line : channel.GetResponseLines()) {
    if (line.find("AirM2M") != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<hal::IModuleHal> CreateAir780eHal() {
  return std::make_unique<Air780eHal>();
}

// Register with module registry
namespace {
struct Air780eRegistrar {
  Air780eRegistrar() {
    ::esp_modem_link::hal::ModuleRegistry::Instance().Register({
        ::esp_modem_link::ModuleType::kAir780E,
        "AIR780E",
        DetectAir780e,
        CreateAir780eHal,
    });
  }
};
static Air780eRegistrar g_air780e_registrar;
}  // namespace

}  // namespace esp_modem_link::modules::air780e
