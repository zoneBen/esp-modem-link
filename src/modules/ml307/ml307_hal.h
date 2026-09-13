#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "at_channel/iat_channel.h"
#include "hal/imodule_hal.h"

namespace esp_modem_link::modules::ml307 {

class Ml307Hal : public hal::IModuleHal {
 public:
  Ml307Hal() = default;
  ~Ml307Hal() override;

  // Module identification
  ModuleType GetModuleType() const override;
  std::string_view GetModuleName() const override;
  const ModuleCapabilities& GetCapabilities() const override;

  // Lifecycle
  Result<> Initialize(at_channel::IAtChannel& channel) override;
  Result<> Reset() override;

  // SIM & Network
  SimState GetSimState() override;
  Result<> ConfigureApn(const ApnConfig& config) override;
  RegistrationState GetRegistrationState() override;
  SignalInfo GetSignalInfo() override;

  // Device info
  Result<std::string> GetImei() override;
  Result<std::string> GetIccid() override;
  Result<std::string> GetRevision() override;
  Result<std::string> GetCarrier() override;

  // Flight mode
  Result<> SetFlightMode(bool enable) override;

  // Low power
  bool SupportsLowPower() const override;
  Result<> EnterSleep(const SleepConfig& config) override;
  Result<> ExitSleep() override;

  // TCP
  Result<int> TcpConnect(std::string_view host,
                         uint16_t port,
                         bool ssl = false) override;
  Result<> TcpClose(int connect_id) override;
  Result<int> TcpSend(int connect_id, const void* data, size_t len) override;

  // UDP
  Result<int> UdpOpen(std::string_view host, uint16_t port) override;
  Result<> UdpClose(int connect_id) override;
  Result<int> UdpSend(int connect_id, const void* data, size_t len) override;

  // Built-in protocols
  bool HasBuiltinHttp() const override;
  bool HasBuiltinMqtt() const override;

 private:
  void RegisterUrcHandlers();
  void UnregisterUrcHandlers();

  int AllocateConnectionId(int first = 0);
  void ReleaseConnectionId(int id);

  // Reads a slot's in-use flag under the pool lock. The flag is written from the
  // URC thread, so it cannot be read directly.
  bool IsConnectionUsed(int id);

  // Allocates a cid, applies the per-socket settings, opens it, and returns the
  // id. Retries past a cid the module has occupied since before this process
  // started; see the definition for why that is not a hard failure.
  Result<int> OpenSocket(std::string_view type,
                         std::string_view host,
                         uint16_t port,
                         bool ssl);

  // The AT+MIPOPEN itself, including the asynchronous result rendezvous. Split
  // out of TcpConnect so the retry above can reuse it unchanged.
  Result<> OpenOnId(int id,
                    std::string_view type,
                    std::string_view host,
                    uint16_t port);

  // Asks the module whether this cid is in use. The pool above tracks what this
  // process opened; the module's view also covers sockets left behind by an
  // earlier run, which is why the two can disagree.
  Result<bool> QuerySocketActive(int id);

  // Brings a cid the module still holds to a released state before it is reused.
  //
  // True means the cid was already free, and is therefore safe to open on. False
  // covers both "the module still holds it" and "it has just been closed": either
  // way the caller must leave it to settle and take another cid, because the
  // events announcing that close name only the cid and can still arrive.
  Result<bool> EnsureSocketFree(int id);

  void OnMipopenUrc(std::string_view command, std::string_view args);
  // Records a "+MIPOPEN: <id>,<code>" result into the open rendezvous. Called
  // both by the URC handler and, when the line was absorbed into the command's
  // own response buffer rather than dispatched, by the code that sent it.
  void RecordOpenResult(std::string_view args);
  void OnMipcloseUrc(std::string_view command, std::string_view args);
  void OnMiprtcpUrc(std::string_view command, std::string_view args);
  void OnMipUrc(std::string_view command, std::string_view args);
  void OnCeregUrc(std::string_view command, std::string_view args);

  Result<std::string> ReadSingleLineResponse(
      std::string_view cmd,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  Result<> ActivatePdp();

  // Socket setup steps shared by TCP and UDP. Ordered per the module's own
  // sequence: TLS switch first, then payload encoding, then MIPOPEN.
  Result<> ConfigureSsl(int id, bool enable);
  Result<> ConfigureEncoding(int id);

  // AT+MIPOPEN answers OK as soon as the request is accepted; the module then
  // reports the real outcome asynchronously as "+MIPOPEN: <id>,<code>". Arm the
  // rendezvous before sending the command and block after it, so a result that
  // arrives while the command is still in flight is not lost.
  void ArmOpenWait(int id);
  Result<> WaitForOpenResult(int id, std::chrono::milliseconds timeout);

  // Sends a payload as inline hex. The module is configured for hex in both
  // directions, which keeps binary data from colliding with AT framing.
  Result<int> SendHexPayload(int connect_id, const void* data, size_t len);

  // Shared by TcpClose and UdpClose, which differ only in which pool entry they
  // free.
  Result<> CloseConnection(int connect_id);

  at_channel::IAtChannel* channel_ = nullptr;

  // URC handles
  std::vector<uint64_t> urc_handles_;

  // Connection ID pool. AT+MIPCFG=? reports "cid",(0-5), so six sockets is the
  // hardware limit, not the eight the datasheet summary implies.
  //
  // A cid is the only thing the module names in its socket events - there is no
  // per-connection handle - so an event for the previous occupant of a cid is
  // indistinguishable from one for the current occupant. Observed on
  // ML307R-DL-MBRH0S01: after a socket was left behind by an earlier process and
  // the reuse of its cid was closed and reopened, a late "disconn" for the old
  // socket arrived after the new connection was up and tore it down mid-response
  // ("connection closed before the response was complete"). Releasing a cid
  // therefore starts a quarantine: it is held out of the rotation long enough for
  // the previous occupant's teardown to have been delivered.
  struct ConnectionSlot {
    bool used = false;
    bool released = false;
    std::chrono::steady_clock::time_point released_at{};
  };
  static constexpr int kMaxConnections = 6;
  ConnectionSlot slots_[kMaxConnections];

  // Guards slots_. The pool is written from the URC thread - a close event
  // releases its cid - and read and written from whichever thread is connecting,
  // so the three fields of a slot cannot be published separately: a reader that
  // caught "free" without the release time would hand out a cid the quarantine
  // exists to keep out of circulation.
  std::mutex pool_mutex_;

  // Long enough to outlast a teardown, short enough that a rotation of six cids
  // is not exhausted by ordinary connect/close traffic. A connect/close per
  // request uses one cid per request and only starts reusing them after a second.
  static constexpr auto kCidQuarantine = std::chrono::milliseconds(1000);

  // How long to wait for a cid to be released after AT+MIPCLOSE, polling
  // AT+MIPSTATE. The close is asynchronous and the module answers OK before the
  // socket is actually gone, so reopening on that OK is what let a close event
  // still in flight tear down the connection that replaced it.
  //
  // The budget is deliberately at least kCidQuarantine: a cid that has not come
  // back within the time the pool would have kept it out of circulation anyway
  // says nothing useful, and giving up earlier would skip cids that were about to
  // settle.
  static constexpr int kClosePollAttempts = 20;
  static constexpr auto kClosePollInterval = std::chrono::milliseconds(50);
  static_assert(kClosePollAttempts * kClosePollInterval >= kCidQuarantine,
                "the close poll must outlast the quarantine it feeds");

  // Confirming a new UART rate, and the hardware reason it is retried rather
  // than read as a refusal ("the module does not answer the first command at a
  // rate it has just switched to", observed at 921600), live with the
  // negotiation itself: at_channel::SetModuleBaudRate.
  //
  // Rendezvous between the connect path and the +MIPOPEN URC handler. The id
  // field doubles as the "someone is waiting" flag so a URC for an unrelated
  // connection id is ignored rather than mistaken for ours.
  struct OpenWait {
    int id = -1;
    int code = 0;
    bool done = false;
  };
  std::mutex open_mutex_;
  std::condition_variable open_cv_;
  OpenWait open_wait_;

  // Cached state
  RegistrationState registration_state_ = RegistrationState::kNotRegistered;
  SignalInfo last_signal_;
};

bool DetectMl307(at_channel::IAtChannel& channel);
std::unique_ptr<hal::IModuleHal> CreateMl307Hal();

// Force linker to include this translation unit (ensures static registrar runs)
void ForceLinkMl307Hal();

}  // namespace esp_modem_link::modules::ml307
