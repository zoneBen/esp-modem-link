#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "at_channel/iat_channel.h"
#include "hal/imodule_hal.h"
#include "platform/itask.h"

namespace esp_modem_link::modules::air780e {

// The 合宙 / SIMCom "CIP" command family, as implemented by
// AirM2M_780EPV_V1004_LTE_AT on an AIR780E.
//
// The shape of this HAL differs from Ml307Hal in three ways that are properties
// of the module rather than choices:
//
//   * Data is pulled, not pushed. AT+CIPRXGET=5 makes the module announce that
//     bytes have arrived ("+CIPRXGET: 1,<id>") without carrying them, and
//     AT+CIPRXGET=3,<id>,<len> fetches them as hex. The push mode
//     (AT+CIPRXGET=0) frames payload as "+RECEIVE,<id>,<len>:" followed by raw
//     binary, which a line-oriented AT layer cannot delimit - a length prefix is
//     not enough when the payload may contain CRLF and "OK". Hex pull keeps the
//     whole channel line-oriented.
//
//   * Connection outcomes are unsolicited lines with no key: "<id>, CONNECT OK",
//     "<id>, CONNECT FAIL", "<id>, ALREADY CONNECT", "<id>, CLOSED". They are
//     caught by an empty-prefix subscription and matched by shape, since the
//     dispatcher only splits a line into key and arguments at a colon.
//
//   * A cid that fails or is closed is immediately reusable - measured on all
//     three paths (clean close, connect failure, peer close) - so there is no
//     quarantine here. AT+CIPSTART on a cid the module still holds answers
//     "+CME ERROR: 65535" and then "<id>, ALREADY CONNECT", which names the
//     problem at the moment of use, so no pre-flight occupancy query is needed
//     either. Both of the queries that suggested themselves are unusable:
//     AT+CIPSTATUS returns OK before its table, and AT+CIPRXGET=4 reports an
//     empty socket and a closed cid identically.
class Air780eHal : public hal::IModuleHal {
 public:
  struct Options {
    // Whether Initialize starts the receive task. A test sets this false and
    // drives DrainPending() itself, because the task wakes on a timer and pulls
    // the very sockets the test is about to assert on - leaving it running would
    // make every such assertion a race against a 100 ms clock. Production leaves
    // it true: nothing else pulls the module's buffer, and an arrival that is
    // never read is a socket that looks stalled.
    bool start_reader = true;
  };

  // Two constructors rather than one with a default argument. "= {}" cannot be
  // written here: building an Options needs the default member initializer
  // above, and the standard does not allow a class's default member initializer
  // to be used in a default argument of the enclosing class - the enclosing
  // class is not complete yet. MSVC takes it, GCC says "could not convert
  // '<brace-enclosed initializer list>()'". Defaulting in the .cpp instead puts
  // the same `{}` where the class *is* complete.
  Air780eHal();
  explicit Air780eHal(Options options);
  ~Air780eHal() override;

  Air780eHal(const Air780eHal&) = delete;
  Air780eHal& operator=(const Air780eHal&) = delete;

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
                         bool ssl = false,
                         const TlsConfig& config = {}) override;
  Result<> TcpClose(int connect_id) override;
  Result<int> TcpSend(int connect_id, const void* data, size_t len) override;

  // UDP
  Result<int> UdpOpen(std::string_view host, uint16_t port) override;
  Result<> UdpClose(int connect_id) override;
  Result<int> UdpSend(int connect_id, const void* data, size_t len) override;

  // Built-in protocols
  bool HasBuiltinHttp() const override;
  bool HasBuiltinMqtt() const override;

  // Runs receive passes over the cids that have data waiting, on the calling
  // thread, until none of them has more. The receive task below drives the same
  // DrainOneCid, so a test that calls this directly is testing the same path
  // rather than a copy of it - and reading a socket becomes a step the test takes
  // instead of a race it runs against a background thread.
  void DrainPending();

 private:
  // AT+CIPMUX=? reports (0-5), so six sockets is the module's limit.
  static constexpr int kMaxConnections = 6;

  // AT+CIPRXGET=? advertises (1-1460) for the read length, but that is not the
  // working ceiling: 750 and 767 are refused with "+CME ERROR: 3" while 720 is
  // served, measured by bisection on this firmware. A refused read consumes no
  // data, so undershooting costs a round trip and nothing else.
  static constexpr int kReadChunk = 720;

  // What one wake-up may pull off a cid before returning to the condition
  // variable. Each chunk is a full command round trip, and the command mutex is
  // held for the length of it, so a socket with a deep buffer must not be able to
  // keep it from the caller that is trying to send. Stopping here does not drop
  // what is left: the pass reports that it stopped and the flag is re-armed, so
  // the remainder is pulled on the next pass.
  static constexpr size_t kMaxDrainPerWake = 8 * 1024;

  // Long enough for the slowest observed outcome and short enough that an
  // unreachable host does not hang the caller forever. Measured: a good host
  // answers CONNECT OK in about 100 ms; a name that will not resolve answers
  // CONNECT FAIL after about 12 s; a routable-looking but unreachable address
  // (192.0.2.1) answers OK and then says nothing at all, ever, which is why this
  // timeout is the only thing that ends that case.
  static constexpr auto kConnectTimeout = std::chrono::milliseconds(30000);

  static constexpr auto kDefaultTimeout = std::chrono::milliseconds(5000);
  static constexpr auto kLongTimeout = std::chrono::milliseconds(30000);

  // "+CME ERROR: 65535" is what AT+CIPSTART answers for a cid the module already
  // holds, and it arrives just before "<id>, ALREADY CONNECT". The code is kept
  // so the connect path can tell that case from every other refusal: it is the
  // one that should move to another cid rather than report a failure.
  static constexpr int kAlreadyConnectedCode = 65535;

  // What AT+CIPSTART can answer. kAlreadyConnected means the module still holds
  // the cid - normally a socket left behind by an earlier process - and is
  // reported so the caller can move to another cid rather than retry this one.
  enum class OpenOutcome {
    kConnected,
    kFailed,
    kAlreadyConnected,
  };

  void RegisterUrcHandlers();
  void UnregisterUrcHandlers();

  // --- cid pool ---

  bool IsConnectionUsed(int id);
  // Whether cid `id` is still the socket that `serial` was handed out for. A cid
  // is reusable the instant it is released, so "is it used" is not enough to
  // decide whether a late event belongs to the socket that is there now.
  bool OwnsSerial(int id, uint32_t serial);
  // Whether the module has already said this cid's peer is gone. A send into such
  // a socket is refused by the module, and the refusal is reported as a lost
  // connection rather than as the AT error it arrives as.
  bool PeerCloseNoted(int id);
  // `avoid` names cids to pass over, so that a search for one that works moves on
  // instead of being handed the same cid back. Null means no preference.
  int AllocateConnectionId(const bool* avoid = nullptr);
  // Gives the slot back and reports the mode whose reservation it was holding, or
  // nullopt when it was holding none. The two are not the same question: a cid
  // allocated during the search for one that works is let go before any
  // reservation has been handed to it, and releasing "whichever mode the slot
  // says" would then take the reservation off a socket that really is holding one.
  std::optional<bool> RetireSlot(int id);
  void ReleaseConnectionId(int id);

  // --- opening ---

  Result<int> OpenSocket(std::string_view type,
                         std::string_view host,
                         uint16_t port,
                         bool ssl,
                         const TlsConfig& config);
  Result<> CloseConnection(int connect_id);

  // The rendezvous for "<id>, CONNECT OK" and its two siblings. Armed before the
  // command goes out, because the outcome can be reported while AT+CIPSTART is
  // still in flight.
  void ArmOpenWait(int id);
  Result<> WaitForOpenResult(int id, std::chrono::milliseconds timeout);

  // --- sending ---

  Result<int> SendPayload(int connect_id, const void* data, size_t len);

  // --- receive ---

  void ReceiveLoop();
  // Returns true when the pass stopped because it hit kMaxDrainPerWake rather than
  // because the socket ran dry - that is, when bytes are still queued and the
  // caller has to come back for them.
  bool DrainOneCid(int id);
  // Ends a socket whose peer has already been reported gone, now that its bytes
  // are exhausted or unreachable. Both halves happen here rather than in the
  // URC handler for the reason the handler cannot drain: see OnUnsolicitedLine.
  void FinishClosedCid(int id);
  // Records that the module has said the peer is gone, without acting on it. The
  // close is held until the socket has been read dry; see the comment on the
  // definition.
  void NotePeerClosed(int id);
  void StopReader();

  // --- identity and network ---

  Result<std::string> ReadSingleLineResponse(
      std::string_view cmd,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
  Result<> ActivatePdp();

  // --- URC handlers ---
  //
  // INVARIANT: these run on the AT channel's receive task. They may record state
  // and dispatch to clients; they must never send a command. AtUart::ProcessLine
  // dispatches every completed line from that task, including lines arriving
  // while a command is in flight, and a command sent from here would block on a
  // mutex held by the thread waiting for a response only this task can deliver.
  void OnCiprxgetUrc(std::string_view args);
  void OnUnsolicitedLine(std::string_view line);
  void RecordOpenResult(int id, OpenOutcome outcome);

  // --- TLS ---
  //
  // AT+CIPSSL is one switch for the whole module, not a per-cid setting, so TLS
  // and plaintext sockets cannot coexist: with the switch on, a connect to a
  // plaintext port answers "CONNECT FAIL", and with it off a TLS port fails the
  // same way. These two counters are what make that a refusal at the API rather
  // than a socket that quietly connects in the wrong mode.
  Result<> ReserveSocketForMode(bool ssl);
  void ReleaseSocketForMode(bool ssl);
  Result<> ApplySslSwitch(bool enable);

  at_channel::IAtChannel* channel_ = nullptr;
  const bool start_reader_;
  std::vector<uint64_t> urc_handles_;

  // Rendezvous between the connect path and the "<id>, CONNECT OK" handler. The
  // id field doubles as the "someone is waiting" flag, so a line for an
  // unrelated cid is ignored rather than mistaken for ours.
  struct OpenWait {
    int id = -1;
    OpenOutcome outcome = OpenOutcome::kFailed;
    bool done = false;
  };
  std::mutex open_mutex_;
  std::condition_variable open_cv_;
  OpenWait open_wait_;

  // Which cids this process holds. The module's view also covers sockets left by
  // an earlier run, which is what AT+CIPSHUT in Initialize clears.
  //
  // The kind of socket is recorded rather than looked up, because two things
  // depend on it and neither is derivable from the module afterwards: a UDP read
  // carries a sender address that has to reach DispatchUdpData, and releasing a
  // socket has to decrement the counter for the mode it was opened in.
  struct SocketSlot {
    bool used = false;
    bool udp = false;
    bool ssl = false;
    // Whether this slot is the one holding the module-wide mode reservation. A
    // slot is reserved for a mode at allocation and only comes to own the
    // reservation when its socket is handed to the caller: the cid search can
    // allocate and let go of several slots first, and those never took one.
    bool holds_mode = false;
    // Bumped on every allocation, so that "this cid" can be told apart from "the
    // socket that happens to hold this cid now". A peer's close can be reported
    // for a cid the application has already let go of and a new socket has taken
    // over, and acting on it then would end a connection that is not the one it
    // was about.
    uint32_t serial = 0;
  };
  mutable std::mutex pool_mutex_;
  SocketSlot slots_[kMaxConnections];
  uint32_t next_serial_ = 1;  // never 0, which is "no socket"

  // Acknowledged sends, per cid. The acknowledgement names only the cid - there
  // is no sequence number to match it to a chunk - so a count is kept and a
  // timed-out send resynchronises it by clearing it: that way a late
  // acknowledgement of an abandoned chunk decrements a count that has already
  // been given up on, instead of being claimed by the chunk that follows.
  struct SendWait {
    int pending = 0;
    bool failed = false;
  };
  std::mutex send_mutex_;
  std::condition_variable send_cv_;
  SendWait send_wait_[kMaxConnections];

  // The sender of the last datagram seen on a cid, as "ip:port". The arrival
  // notification carries it and so does the read reply; this is the fallback for
  // the read that comes back without one, which would otherwise leave a datagram
  // with nothing to attribute it to. Guarded by rx_mutex_.
  std::string rx_peer_[kMaxConnections];

  // Cids with data waiting to be pulled. Written by the "+CIPRXGET: 1" handler
  // on the receive task, read and cleared by the receive loop.
  std::mutex rx_mutex_;
  std::condition_variable rx_cv_;
  bool rx_pending_[kMaxConnections] = {};
  // The serial of the socket a peer close was reported for, or 0 for none. The
  // close is held here rather than acted on where it arrives, because the module
  // can report it while bytes it has already taken in are still queued for us.
  uint32_t rx_close_serial_[kMaxConnections] = {};
  bool rx_stop_ = false;
  std::unique_ptr<platform::ITask> rx_task_;

  // How many open sockets of each kind, and whether the module-wide switch is
  // mid-change. Guarded by ssl_mutex_, which is deliberately NOT held while
  // AT+CIPSSL is being sent: a URC handler that releases a socket takes this
  // same mutex, and it runs on the receive task - the one that has to deliver
  // the reply to that very command. ssl_switch_in_progress_ is what lets the
  // mutex be dropped mid-switch without a second opener starting another one.
  std::mutex ssl_mutex_;
  std::condition_variable ssl_cv_;
  int ssl_sockets_open_ = 0;
  int plain_sockets_open_ = 0;
  bool ssl_switch_in_progress_ = false;

  // The two commands answered with an event rather than a response line:
  // AT+CIPSHUT answers "SHUT OK" and AT+CIPCLOSE answers "<id>, CLOSE OK". Each
  // is armed before the command goes out and matched by value on the way back,
  // because the module may report either for a reason other than this call's -
  // a stale CLOSE OK for a cid a previous close already dealt with.
  std::mutex event_mutex_;
  std::condition_variable event_cv_;
  bool shut_ok_ = false;
  bool close_ok_ = false;
  int close_pending_id_ = -1;

  // Cached state
  RegistrationState registration_state_ = RegistrationState::kNotRegistered;
  SignalInfo last_signal_;
};

bool DetectAir780e(at_channel::IAtChannel& channel);
std::unique_ptr<hal::IModuleHal> CreateAir780eHal();

// Force linker to include this translation unit (ensures static registrar runs)
void ForceLinkAir780eHal();

}  // namespace esp_modem_link::modules::air780e
