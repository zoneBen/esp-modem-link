#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include "esp_modem_link/callbacks.h"
#include "esp_modem_link/config_types.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

// The close codes this library names. It is not a closed set: a peer may send
// any code in the ranges the protocol allows - the one reserved for
// applications, and any registered after this was written - so a code that is
// not named below is reported as the number it was rather than being coerced
// onto the nearest member or dropped.
enum class WebSocketCloseCode : uint16_t {
  kNormal = 1000,
  kGoingAway = 1001,
  kProtocolError = 1002,
  kUnsupportedData = 1003,
  kNoStatus = 1005,
  kAbnormalClosure = 1006,
  kInvalidPayloadData = 1007,
  kPolicyViolation = 1008,
  kMessageTooBig = 1009,
  kMandatoryExtension = 1010,
  kInternalError = 1011,
};

// A message, with the kind the sender gave it. The flag is what a text frame
// and a binary one cannot be told apart without, and Send()/SendFragment() let
// a sender say which it is sending, so a receiver that could not see it would
// be the asymmetric half of the pair. For a message that arrived in fragments
// it is the kind the frame that opened it carried: the continuation frames
// that follow carry none.
using WebSocketMessageCallback =
    std::function<void(std::string_view data, bool binary)>;

// How the connection ended: the peer's code when it sent one, and the code this
// side chose when it did not. 1005 and 1006 are reported here and are never
// sent on the wire - that is what they mean.
using WebSocketCloseCallback =
    std::function<void(WebSocketCloseCode code, std::string_view reason)>;

class WebSocketClient {
 public:
  virtual ~WebSocketClient() = default;

  virtual void SetHeader(std::string_view key, std::string_view value) = 0;

  // Settings for the handshake under wss://. Unlike MQTT, this client reads the
  // caller's intent from the URL - ws:// against wss:// - so this configures a
  // TLS connection that the scheme has already asked for rather than switching
  // one on. It is also unlike HTTP, which can follow a redirect across the
  // boundary and therefore re-reads the config per request: a WebSocket
  // connection is opened by a single upgrade whose URL is known in advance.
  //
  // Read when the connection is established, so a change applies from the next
  // Connect() and not to an open socket, which is already past its handshake.
  // Ignored entirely by a ws:// connection.
  virtual void SetTlsConfig(const TlsConfig& config) = 0;

  // Off unless `enabled` is set, and off at a zero interval whatever it says: a
  // ping every zero seconds is a loop rather than a heartbeat. The timeout is
  // how long the peer has to answer a ping before the connection is treated as
  // dead; a zero timeout means the interval is used instead, so a caller who
  // names only an interval still gets a deadline.
  //
  // Each call replaces the whole config: a field left out goes back to the
  // default below, `enabled` included. Naming a field with a designated
  // initialiser is how a caller changes one thing and leaves the rest alone -
  // `SetHeartbeat({.interval = 5s, .enabled = true})` - where naming only the
  // one that changed would turn the heartbeat off rather than retune it.
  struct HeartbeatConfig {
    std::chrono::seconds interval{30};
    std::chrono::seconds timeout{10};
    bool enabled = false;
  };
  virtual void SetHeartbeat(const HeartbeatConfig& config) = 0;

  // Off unless `enabled` is set. A connection that drops or fails its heartbeat
  // is then re-established on this schedule: the wait starts at `initial_delay`
  // and is multiplied by `backoff_factor` after each failed attempt, up to
  // `max_delay`. A factor of one or less leaves the wait constant rather than
  // shrinking it, and a start of zero means retry at once. `max_retries` bounds
  // how many consecutive attempts are made, and -1 means no bound.
  //
  // Each call replaces the whole config, as SetHeartbeat does.
  struct ReconnectConfig {
    bool enabled = false;
    int max_retries = -1;
    std::chrono::milliseconds initial_delay{1000};
    float backoff_factor = 2.0f;
    std::chrono::milliseconds max_delay{30000};
  };
  virtual void SetAutoReconnect(const ReconnectConfig& config) = 0;

  virtual Result<> Connect(std::string_view url) = 0;
  virtual void Close(WebSocketCloseCode code = WebSocketCloseCode::kNormal,
                     std::string_view reason = "") = 0;

  virtual Result<> Send(std::string_view data, bool binary = false) = 0;
  virtual Result<> SendFragment(const void* data,
                                size_t len,
                                bool binary,
                                bool fin) = 0;
  virtual void Ping(std::string_view payload = "") = 0;

  void OnConnected(EventCallback callback) {
    on_connected_ = std::move(callback);
  }
  void OnDisconnected(WebSocketCloseCallback callback) {
    on_disconnected_ = std::move(callback);
  }
  void OnMessage(WebSocketMessageCallback callback) {
    on_message_ = std::move(callback);
  }
  void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }
  void OnPong(DataCallback callback) { on_pong_ = std::move(callback); }

  bool IsConnected() const { return connected_.load(); }

 protected:
  EventCallback on_connected_;
  WebSocketCloseCallback on_disconnected_;
  WebSocketMessageCallback on_message_;
  ErrorCallback on_error_;
  DataCallback on_pong_;
  // Atomic because the engine sets it from the transport's receive thread and
  // the application reads it from its own.
  std::atomic<bool> connected_{false};
};

}  // namespace esp_modem_link
