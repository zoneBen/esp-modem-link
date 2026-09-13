#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_modem_link/config_types.h"
#include "esp_modem_link/tcp_client.h"

namespace esp_modem_link::testing {

class MockTcpClient;

// What a transport recorded while it was alive. Kept apart from the transport
// itself because the software clients are entitled to destroy their transport -
// at the end of a non-keep-alive request, and again on Close() - and the test
// still has to see what happened first.
struct MockState {
  int connect_calls = 0;
  int disconnect_calls = 0;
  std::string last_host;
  uint16_t last_port = 0;
  bool was_tls = false;
  TlsConfig last_tls_config;
  // Everything sent over this connection, and the same bytes split at Send()
  // boundaries. A protocol that has to frame its own messages is asserted
  // against the second: one packet per call is part of the contract, not an
  // accident of how the caller happened to call Send().
  std::string sent;
  std::vector<std::string> sends;
  // Incremented on every Send(). Atomic because a client with a task of its own
  // - the MQTT keep-alive - sends from another thread, and this is the only
  // member a test may read while that task is running. `sent` and `sends` are
  // for reading once nothing else is sending.
  std::atomic<int> send_count{0};
  // The transport handed out most recently, so a test can push bytes into a
  // connection the client owns. Cleared when that transport is destroyed.
  MockTcpClient* last_client = nullptr;
};

// A TcpClient that answers a request as soon as it is sent. Responding
// synchronously from Send() keeps these tests deterministic while still
// exercising the real path: the data callback is invoked from outside the
// client's own code, exactly as the AT layer's receive thread does it.
//
// Server-initiated traffic - a broker pushing a PUBLISH, a WebSocket peer
// sending a frame - has no request to answer, so it is delivered with
// Deliver() instead.
class MockTcpClient : public TcpClient {
 public:
  struct Script {
    std::string response;
    bool respond = false;
    // One response per Send(), in order, for a connection that carries more
    // than one request - a redirect chain over a keep-alive socket. The last
    // entry repeats for any send past the end of the list, so a test lists only
    // as many as it has something to say about. When this is non-empty it takes
    // the place of `response`.
    std::vector<std::string> sequence;
    // Answer the first send only. A peer that is not answering request for
    // request - a WebSocket server, which speaks once the upgrade is done and
    // then pushes frames of its own accord - has to be staged this way, or
    // every frame the client sends is answered with a second copy of the
    // handshake.
    bool respond_once = false;
    bool close_after = false;
    size_t chunk_size = 0;  // 0 = deliver the response in one piece
    bool fail_connect = false;
    bool fail_send = false;
    // Fail the send at this index (0-based) and every one after it, rather than
    // all of them. A client that sends a head and then a body has two paths
    // through the same Send(), and only the second is the one under test - with
    // fail_send alone the head would fail first and the body never be reached.
    // SIZE_MAX leaves every send alone.
    size_t fail_send_at = SIZE_MAX;
  };

  MockTcpClient(std::shared_ptr<MockState> state, const Script& script)
      : state_(std::move(state)), script_(script) {
    // The constructor is where the state learns about the transport, because
    // the client builds it and hands it straight to itself.
    state_->last_client = this;
  }

  ~MockTcpClient() override {
    // A test reaches the live transport through the state, and the client may
    // destroy that transport at any point. Clearing the pointer here is what
    // keeps that shortcut from naming freed memory.
    if (state_->last_client == this) {
      state_->last_client = nullptr;
    }
  }

  Result<> Connect(std::string_view host, uint16_t port) override {
    state_->connect_calls++;
    state_->last_host = std::string(host);
    state_->last_port = port;
    if (script_.fail_connect) {
      return std::unexpected(
          NetworkError(NetworkErrc::kConnectFailed, 0, "mock connect failed"));
    }
    connected_ = true;
    return {};
  }

  void Disconnect() override {
    connected_ = false;
    state_->disconnect_calls++;
  }

  Result<int> Send(const void* data, size_t len) override {
    const std::string bytes(static_cast<const char*>(data), len);
    state_->sent.append(bytes);
    state_->sends.push_back(bytes);
    const size_t index = static_cast<size_t>(state_->send_count++);
    if (script_.fail_send || index >= script_.fail_send_at) {
      return std::unexpected(
          NetworkError(NetworkErrc::kTransmitFailed, 0, "mock send failed"));
    }
    EmitResponse();
    return static_cast<int>(len);
  }

  // Bytes arriving from the peer with no Send() to answer. Returns false when
  // the client has already let go of its callbacks, which is the observable
  // difference between a transport still in use and one being torn down.
  bool Deliver(std::string_view data) {
    if (!on_data_) return false;
    on_data_(data);
    return true;
  }

  // The peer closing the connection.
  bool DeliverClose() {
    if (!on_disconnected_) return false;
    on_disconnected_();
    return true;
  }

 private:
  void EmitResponse() {
    if (!on_data_) return;

    std::string response;
    if (!script_.sequence.empty()) {
      response = script_.sequence.front();
      if (script_.sequence.size() > 1) script_.sequence.erase(
          script_.sequence.begin());
    } else if (script_.respond) {
      if (script_.respond_once) script_.respond = false;
      response = script_.response;
    } else {
      return;
    }

    if (script_.chunk_size == 0) {
      on_data_(response);
    } else {
      std::string_view whole(response);
      for (size_t i = 0; i < whole.size(); i += script_.chunk_size) {
        on_data_(whole.substr(i, script_.chunk_size));
      }
    }
    if (script_.close_after && on_disconnected_) {
      on_disconnected_();
    }
  }

  std::shared_ptr<MockState> state_;
  Script script_;
};

}  // namespace esp_modem_link::testing
