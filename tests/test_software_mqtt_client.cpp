#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "mock_tcp_client.h"
#include "protocol/mqtt/mqtt_packet.h"
#include "protocol/mqtt/software_mqtt_client.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;
using namespace esp_modem_link::testing;

namespace {

// A packet the broker sends that carries nothing but a packet id.
std::string BrokerAck(MqttPacketType type, uint16_t packet_id) {
  std::string body;
  body.push_back(static_cast<char>(packet_id >> 8));
  body.push_back(static_cast<char>(packet_id & 0xFF));
  return EncodePacket(MqttFixedHeader{type}, body);
}

std::string BrokerConnack(uint8_t return_code) {
  // Session-present is zero: the broker has no session of ours to resume.
  std::string body("\x00", 1);
  body.push_back(static_cast<char>(return_code));
  return EncodePacket(MqttFixedHeader{MqttPacketType::kConnack}, body);
}

std::string BrokerPublish(std::string_view topic,
                          std::string_view payload,
                          uint8_t qos,
                          uint16_t packet_id = 0) {
  std::string body;
  body.push_back(static_cast<char>(topic.size() >> 8));
  body.push_back(static_cast<char>(topic.size() & 0xFF));
  body.append(topic);
  if (qos > 0) {
    body.push_back(static_cast<char>(packet_id >> 8));
    body.push_back(static_cast<char>(packet_id & 0xFF));
  }
  body.append(payload);

  MqttFixedHeader header;
  header.type = MqttPacketType::kPublish;
  header.qos = qos;
  return EncodePacket(header, body);
}

std::string BrokerSuback(uint16_t packet_id, uint8_t return_code) {
  std::string body;
  body.push_back(static_cast<char>(packet_id >> 8));
  body.push_back(static_cast<char>(packet_id & 0xFF));
  body.push_back(static_cast<char>(return_code));
  return EncodePacket(MqttFixedHeader{MqttPacketType::kSuback}, body);
}

// The first byte of a packet: the type in its high nibble and the flags in its
// low one. For a packet whose flags are fixed, that byte names it exactly, which
// is the whole of what most of these tests have to say about what went out.
uint8_t FirstByte(std::string_view packet) {
  return static_cast<uint8_t>(packet[0]);
}

uint8_t ByteAt(std::string_view packet, size_t index) {
  return static_cast<uint8_t>(packet[index]);
}

bool WaitFor(const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

// The broker's answers arrive on the socket rather than inside the write, so
// unlike the HTTP tests these drive responses with Deliver() and keep the
// script for the CONNACK alone - which Connect() cannot be told about any other
// way, because it is blocked waiting for it.
class ClientUnderTest {
 public:
  explicit ClientUnderTest(
      std::chrono::milliseconds connack_timeout = std::chrono::seconds(10)) {
    client_ = std::make_unique<SoftwareMqttClient>(
        [this](bool tls, const TlsConfig& config)
            -> Result<std::unique_ptr<TcpClient>> {
          auto state = std::make_shared<MockState>();
          state->was_tls = tls;
          state->last_tls_config = config;
          state_.push_back(state);

          MockTcpClient::Script script = script_;
          if (fail_connect_) script.fail_connect = true;
          return std::unique_ptr<TcpClient>(
              std::make_unique<MockTcpClient>(std::move(state), script));
        },
        connack_timeout);
  }

  SoftwareMqttClient& client() { return *client_; }

  // The answer the transport gives to anything sent over it. Only the CONNACK
  // is staged this way; every later packet is delivered explicitly.
  void RespondWith(std::string response) {
    script_.response = std::move(response);
    script_.respond = true;
  }

  void SetFailConnect(bool enable) { fail_connect_ = enable; }

  // A transport that answers the CONNECT. It answers everything else with the
  // same CONNACK, which the client ignores because no connection is in progress
  // by then - so one script covers the harness.
  void AcceptConnections() { RespondWith(BrokerConnack(0)); }

  void Connect() {
    ASSERT_TRUE(client_->Connect("broker.example", 1883).has_value());
  }

  void ConnectAndAccept() {
    AcceptConnections();
    Connect();
    ASSERT_TRUE(client_->IsConnected());
  }

  // The live transport, through which a test pushes broker traffic.
  MockTcpClient& transport(size_t index = 0) {
    MockTcpClient* mock = state_.at(index)->last_client;
    // Thrown rather than dereferenced: a test that reaches for a transport the
    // client has already let go of should say so, not crash the runner.
    if (mock == nullptr) throw std::runtime_error("no live transport");
    return *mock;
  }

  MockState& state(size_t index = 0) { return *state_.at(index); }

  // Reading what was sent is only safe once nothing else is sending, which is
  // true after Connect/Disconnect have returned and the keep-alive task is
  // parked on an outstanding PINGREQ.
  const std::vector<std::string>& sends(size_t index = 0) {
    return state_.at(index)->sends;
  }

  size_t transport_count() const { return state_.size(); }

 private:
  std::unique_ptr<SoftwareMqttClient> client_;
  std::vector<std::shared_ptr<MockState>> state_;
  MockTcpClient::Script script_;
  bool fail_connect_ = false;
};

// --- Connecting -----------------------------------------------------------

TEST(SoftwareMqttClientTest, ConnectOpensTheTransportAndSendsAConnect) {
  ClientUnderTest test;
  test.AcceptConnections();
  test.Connect();

  ASSERT_EQ(test.transport_count(), 1u);
  EXPECT_EQ(test.state().last_host, "broker.example");
  EXPECT_EQ(test.state().last_port, 1883);
  ASSERT_EQ(test.sends().size(), 1u);
  EXPECT_EQ(FirstByte(test.sends()[0]), 0x10);
  EXPECT_TRUE(test.client().IsConnected());
}

TEST(SoftwareMqttClientTest, TheSessionIsNotConnectedUntilTheBrokerAccepts) {
  ClientUnderTest test(std::chrono::milliseconds(300));
  // Nothing answers the CONNECT, so the wait runs out.
  auto result = test.client().Connect("broker.example", 1883);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kTimeout);
  EXPECT_FALSE(test.client().IsConnected());
}

TEST(SoftwareMqttClientTest, ARefusalIsReportedAsAnAuthenticationFailure) {
  ClientUnderTest test;
  test.RespondWith(BrokerConnack(5));  // 5 = not authorized

  auto result = test.client().Connect("broker.example", 1883);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kMqttNotAuthorized);
  // The broker's own code survives, because which of the six refusals it was is
  // a different problem to go and fix.
  EXPECT_EQ(result.error().native, 5);
  EXPECT_FALSE(test.client().IsConnected());
}

TEST(SoftwareMqttClientTest, ARefusalThatIsNotAboutCredentialsIsNotOne) {
  ClientUnderTest test;
  test.RespondWith(BrokerConnack(3));  // 3 = server unavailable

  auto result = test.client().Connect("broker.example", 1883);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectFailed);
  EXPECT_EQ(result.error().native, 3);
}

TEST(SoftwareMqttClientTest, ConnectFailureIsPropagated) {
  ClientUnderTest test;
  test.SetFailConnect(true);

  auto result = test.client().Connect("broker.example", 1883);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectFailed);
}

TEST(SoftwareMqttClientTest, ASecondConnectIsRefusedWhileTheFirstIsUp) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  auto result = test.client().Connect("other.example", 1883);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kAlreadyConnected);
  EXPECT_EQ(test.transport_count(), 1u);
}

TEST(SoftwareMqttClientTest, TheConnectPacketCarriesWhatWasConfigured) {
  ClientUnderTest test;
  test.client().SetClientId("device-7");
  test.client().SetCredentials("user", "secret");
  test.client().SetWill(MqttWill{"will/topic", "gone", 1, true});
  test.AcceptConnections();
  test.Connect();

  std::string buffer = test.sends()[0];
  auto packet = TakePacket(buffer);
  ASSERT_TRUE(packet.has_value());
  ASSERT_EQ(packet->header.type, MqttPacketType::kConnect);

  // Flag byte: username, password, will retain, will QoS 1, will, clean
  // session - every one of them asked for.
  size_t offset = 0;
  ASSERT_TRUE(ReadString(packet->body, offset).has_value());  // protocol name
  EXPECT_EQ(ByteAt(packet->body, offset), 4);                 // protocol level
  EXPECT_EQ(ByteAt(packet->body, offset + 1),
            0x80 | 0x40 | 0x20 | 0x08 | 0x04 | 0x02);

  EXPECT_NE(packet->body.find("device-7"), std::string::npos);
  EXPECT_NE(packet->body.find("will/topic"), std::string::npos);
  EXPECT_NE(packet->body.find("user"), std::string::npos);
  EXPECT_NE(packet->body.find("secret"), std::string::npos);
}

// MqttClient has no scheme in Connect() to read the caller's intent from, so
// the TLS settings themselves are the signal.
TEST(SoftwareMqttClientTest, TlsIsOffUntilAConfigIsGiven) {
  ClientUnderTest plain;
  plain.AcceptConnections();
  plain.Connect();
  EXPECT_FALSE(plain.state().was_tls);

  ClientUnderTest secure;
  TlsConfig config;
  config.verify_certificate = false;
  config.alpn_protocols = "mqtt";
  secure.client().SetTlsConfig(config);
  secure.AcceptConnections();
  secure.Connect();

  EXPECT_TRUE(secure.state().was_tls);
  EXPECT_FALSE(secure.state().last_tls_config.verify_certificate);
  EXPECT_EQ(secure.state().last_tls_config.alpn_protocols, "mqtt");
}

// --- Publishing -----------------------------------------------------------

TEST(SoftwareMqttClientTest, PublishingBeforeConnectingFails) {
  ClientUnderTest test;
  auto result = test.client().Publish("topic", "payload");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotConnected);
  EXPECT_EQ(test.transport_count(), 0u);
}

TEST(SoftwareMqttClientTest, QoS0PublishesWithoutAPacketIdAndReturnsZero) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  auto result = test.client().Publish("topic", "payload", MqttQoS::kQoS0);
  ASSERT_TRUE(result.has_value());
  // Nothing will acknowledge it, so there is no id to report.
  EXPECT_EQ(result.value(), 0);

  std::string buffer = test.sends()[1];
  auto packet = TakePacket(buffer);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->header.qos, 0);
  auto message = ParsePublish(packet->header, packet->body);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->topic, "topic");
  EXPECT_EQ(message->payload, "payload");
}

TEST(SoftwareMqttClientTest, QoS1PublishesUnderAnIdAndReportsTheAcknowledgement) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::vector<int> acknowledged;
  test.client().OnPublishComplete(
      [&](int msg_id) { acknowledged.push_back(msg_id); });

  auto result = test.client().Publish("topic", "payload", MqttQoS::kQoS1);
  ASSERT_TRUE(result.has_value());
  const int packet_id = result.value();
  EXPECT_GT(packet_id, 0);

  std::string buffer = test.sends()[1];
  auto packet = TakePacket(buffer);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->header.qos, 1);
  auto message = ParsePublish(packet->header, packet->body);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->packet_id, packet_id);

  // Nothing is acknowledged until the broker says so.
  EXPECT_TRUE(acknowledged.empty());

  ASSERT_TRUE(test.transport().Deliver(
      BrokerAck(MqttPacketType::kPuback, static_cast<uint16_t>(packet_id))));
  ASSERT_EQ(acknowledged.size(), 1u);
  EXPECT_EQ(acknowledged[0], packet_id);
}

TEST(SoftwareMqttClientTest, QoS2WalksTheWholeHandshake) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  bool acknowledged = false;
  test.client().OnPublishComplete([&](int) { acknowledged = true; });

  auto result = test.client().Publish("topic", "payload", MqttQoS::kQoS2);
  ASSERT_TRUE(result.has_value());
  const auto packet_id = static_cast<uint16_t>(result.value());

  // PUBREC from the broker is answered with PUBREL, not with the ack callback:
  // the message is not the broker's yet.
  ASSERT_TRUE(test.transport().Deliver(
      BrokerAck(MqttPacketType::kPubrec, packet_id)));
  ASSERT_EQ(test.sends().size(), 3u);
  EXPECT_EQ(FirstByte(test.sends()[2]), 0x60 | 0x02);  // PUBREL
  EXPECT_FALSE(acknowledged);

  ASSERT_TRUE(test.transport().Deliver(
      BrokerAck(MqttPacketType::kPubcomp, packet_id)));
  EXPECT_TRUE(acknowledged);
}

TEST(SoftwareMqttClientTest, EachQoS1PublishGetsItsOwnPacketId) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  auto first = test.client().Publish("topic", "one", MqttQoS::kQoS1);
  auto second = test.client().Publish("topic", "two", MqttQoS::kQoS1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  // Acknowledgement identifies the message by this id, so two in flight must
  // not share one.
  EXPECT_NE(first.value(), second.value());
  EXPECT_GT(first.value(), 0);
}

// --- Subscribing ----------------------------------------------------------

TEST(SoftwareMqttClientTest, SubscribeSendsTheFilterAndReturnsItsPacketId) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  auto result = test.client().Subscribe("sensors/#", MqttQoS::kQoS1);
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result.value(), 0);

  std::string buffer = test.sends()[1];
  auto packet = TakePacket(buffer);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->header.type, MqttPacketType::kSubscribe);
  // Flags are fixed at 0b0010 for SUBSCRIBE; a broker may drop the connection
  // over anything else.
  EXPECT_EQ(ByteAt(test.sends()[1], 0) & 0x0F, 0x02);
  EXPECT_NE(packet->body.find("sensors/#"), std::string::npos);
}

TEST(SoftwareMqttClientTest, ASubackThatRefusesTheFilterIsReported) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::vector<NetworkError> errors;
  test.client().OnError([&](const NetworkError& error) {
    errors.push_back(error);
  });

  auto result = test.client().Subscribe("forbidden/#", MqttQoS::kQoS1);
  ASSERT_TRUE(result.has_value());

  ASSERT_TRUE(test.transport().Deliver(
      BrokerSuback(static_cast<uint16_t>(result.value()), 0x80)));
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].code, NetworkErrc::kProtocolError);
  // The topic is named, because a client with several subscriptions needs to
  // know which one is not coming.
  EXPECT_NE(errors[0].context.find("forbidden/#"), std::string::npos);
}

TEST(SoftwareMqttClientTest, ASubackThatGrantsTheFilterIsNotAnError) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  int errors = 0;
  test.client().OnError([&](const NetworkError&) { ++errors; });

  auto result = test.client().Subscribe("sensors/#", MqttQoS::kQoS1);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(test.transport().Deliver(
      BrokerSuback(static_cast<uint16_t>(result.value()), 0x01)));
  EXPECT_EQ(errors, 0);
}

TEST(SoftwareMqttClientTest, UnsubscribeSendsTheFilter) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  ASSERT_TRUE(test.client().Unsubscribe("sensors/#").has_value());

  std::string buffer = test.sends()[1];
  auto packet = TakePacket(buffer);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->header.type, MqttPacketType::kUnsubscribe);
  EXPECT_NE(packet->body.find("sensors/#"), std::string::npos);
}

// --- Inbound messages -----------------------------------------------------

TEST(SoftwareMqttClientTest, AnInboundQoS0PublishReachesOnMessage) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::string topic;
  std::string payload;
  test.client().OnMessage([&](std::string_view t, std::string_view p) {
    topic = std::string(t);
    payload = std::string(p);
  });

  ASSERT_TRUE(test.transport().Deliver(BrokerPublish("a/b", "hello", 0)));

  EXPECT_EQ(topic, "a/b");
  EXPECT_EQ(payload, "hello");
  // QoS 0 is not acknowledged: there is nothing to acknowledge.
  EXPECT_EQ(test.sends().size(), 1u);
}

TEST(SoftwareMqttClientTest, AnInboundQoS1PublishIsAcknowledgedAndDelivered) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::string payload;
  test.client().OnMessage(
      [&](std::string_view, std::string_view p) { payload = std::string(p); });

  ASSERT_TRUE(test.transport().Deliver(BrokerPublish("a/b", "hello", 1, 9)));

  EXPECT_EQ(payload, "hello");
  ASSERT_EQ(test.sends().size(), 2u);
  EXPECT_EQ(FirstByte(test.sends()[1]), 0x40);  // PUBACK
  auto packet = TakePacket(*const_cast<std::string*>(&test.sends()[1]));
  ASSERT_TRUE(packet.has_value());
  auto id = ParsePacketId(packet->body);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 9);
}

// Exactly-once: the message is the broker's until it says PUBREL, and until
// then nobody has seen it.
TEST(SoftwareMqttClientTest, AnInboundQoS2PublishIsHeldUntilPubrel) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::vector<std::string> received;
  test.client().OnMessage([&](std::string_view, std::string_view p) {
    received.push_back(std::string(p));
  });

  ASSERT_TRUE(test.transport().Deliver(BrokerPublish("a/b", "hello", 2, 9)));

  EXPECT_TRUE(received.empty());
  ASSERT_EQ(test.sends().size(), 2u);
  EXPECT_EQ(FirstByte(test.sends()[1]), 0x50);  // PUBREC

  ASSERT_TRUE(test.transport().Deliver(BrokerAck(MqttPacketType::kPubrel, 9)));
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0], "hello");
  ASSERT_EQ(test.sends().size(), 3u);
  EXPECT_EQ(FirstByte(test.sends()[2]), 0x70);  // PUBCOMP
}

// A broker that never heard the PUBREC sends the PUBLISH again. Delivering on
// the second PUBLISH would make the QoS 2 guarantee a QoS 1 one.
TEST(SoftwareMqttClientTest, ARepeatedQoS2PublishIsNotDeliveredTwice) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  int received = 0;
  test.client().OnMessage([&](std::string_view, std::string_view) {
    ++received;
  });

  ASSERT_TRUE(test.transport().Deliver(BrokerPublish("a/b", "hello", 2, 9)));
  ASSERT_TRUE(test.transport().Deliver(BrokerPublish("a/b", "hello", 2, 9)));
  // Both PUBLISHes are answered, because the repeat means the PUBREC was lost.
  EXPECT_EQ(test.sends().size(), 3u);

  ASSERT_TRUE(test.transport().Deliver(BrokerAck(MqttPacketType::kPubrel, 9)));
  EXPECT_EQ(received, 1);
}

TEST(SoftwareMqttClientTest, APubrelForAnUnknownIdIsAnsweredWithoutDelivering) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  int received = 0;
  test.client().OnMessage([&](std::string_view, std::string_view) {
    ++received;
  });

  ASSERT_TRUE(test.transport().Deliver(BrokerAck(MqttPacketType::kPubrel, 77)));

  // Answered, or the broker will keep resending it - but nothing is delivered
  // that the client never received.
  ASSERT_EQ(test.sends().size(), 2u);
  EXPECT_EQ(FirstByte(test.sends()[1]), 0x70);  // PUBCOMP
  EXPECT_EQ(received, 0);
}

TEST(SoftwareMqttClientTest, TwoPacketsInOneReadAreBothDelivered) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::vector<std::string> received;
  test.client().OnMessage([&](std::string_view, std::string_view p) {
    received.push_back(std::string(p));
  });

  ASSERT_TRUE(test.transport().Deliver(BrokerPublish("a/b", "one", 0) +
                                       BrokerPublish("a/b", "two", 0)));

  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(received[0], "one");
  EXPECT_EQ(received[1], "two");
}

TEST(SoftwareMqttClientTest, APacketSplitAcrossTwoReadsIsDeliveredOnce) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::vector<std::string> received;
  test.client().OnMessage([&](std::string_view, std::string_view p) {
    received.push_back(std::string(p));
  });

  const std::string packet = BrokerPublish("a/b", "hello", 0);
  ASSERT_TRUE(test.transport().Deliver(packet.substr(0, 3)));
  EXPECT_TRUE(received.empty());
  ASSERT_TRUE(test.transport().Deliver(packet.substr(3)));

  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0], "hello");
}

// A stream whose length field never completes would otherwise be buffered
// forever, since nothing about it says it is malformed rather than slow.
TEST(SoftwareMqttClientTest, AnOversizedInboundStreamEndsTheConnection) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::vector<NetworkError> errors;
  test.client().OnError(
      [&](const NetworkError& error) { errors.push_back(error); });

  // One fixed header promising far more than the client will hold.
  std::string flood = "\x30\xFF\xFF\xFF\x7F";
  flood.append(70 * 1024, 'x');
  ASSERT_TRUE(test.transport().Deliver(flood));

  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].code, NetworkErrc::kBufferOverflow);
  EXPECT_FALSE(test.client().IsConnected());
}

// --- Closing --------------------------------------------------------------

TEST(SoftwareMqttClientTest, DisconnectSaysGoodbyeAndReportsItOnce) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  int disconnects = 0;
  test.client().OnDisconnected([&] { ++disconnects; });

  test.client().Disconnect();

  EXPECT_EQ(disconnects, 1);
  EXPECT_FALSE(test.client().IsConnected());
  ASSERT_GE(test.sends().size(), 2u);
  EXPECT_EQ(FirstByte(test.sends().back()), 0xE0);  // DISCONNECT
  EXPECT_EQ(test.state().disconnect_calls, 1);
}

TEST(SoftwareMqttClientTest, ASecondDisconnectIsSilent) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  int disconnects = 0;
  test.client().OnDisconnected([&] { ++disconnects; });

  test.client().Disconnect();
  test.client().Disconnect();

  // There is nothing left to close the second time, and reporting a
  // disconnection that did not happen would be a lie.
  EXPECT_EQ(disconnects, 1);
}

TEST(SoftwareMqttClientTest, APeerCloseReportsTheDisconnection) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  int disconnects = 0;
  test.client().OnDisconnected([&] { ++disconnects; });

  ASSERT_TRUE(test.transport().DeliverClose());

  EXPECT_EQ(disconnects, 1);
  EXPECT_FALSE(test.client().IsConnected());
}

TEST(SoftwareMqttClientTest, ADestroyedClientClosesItsTransport) {
  int disconnects = 0;
  {
    ClientUnderTest test;
    test.ConnectAndAccept();
    test.client().OnDisconnected([&] { ++disconnects; });
    EXPECT_EQ(test.state().disconnect_calls, 0);
  }
  // Destroying the client has to close the socket rather than drop it, or the
  // module keeps a connection nobody is reading.
  EXPECT_EQ(disconnects, 1);
}

// --- Keep-alive -----------------------------------------------------------

TEST(SoftwareMqttClientTest, AnIdleConnectionGetsAPingreq) {
  ClientUnderTest test;
  test.client().SetKeepAlive(1);
  test.ConnectAndAccept();
  ASSERT_EQ(test.state().send_count.load(), 1);  // the CONNECT

  ASSERT_TRUE(WaitFor([&] { return test.state().send_count.load() >= 2; },
                      std::chrono::seconds(4)));

  // Stopped before the log is read, so no other thread can be writing to it.
  test.client().Disconnect();
  ASSERT_GE(test.sends().size(), 2u);
  EXPECT_EQ(FirstByte(test.sends()[1]), 0xC0);  // PINGREQ
}

TEST(SoftwareMqttClientTest, KeepAliveZeroSendsNothingAtAll) {
  ClientUnderTest test;
  test.client().SetKeepAlive(0);
  test.ConnectAndAccept();

  // Longer than the interval that would have produced a PINGREQ had keep-alive
  // been on.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  test.client().Disconnect();
  // The CONNECT and the DISCONNECT, and nothing in between.
  EXPECT_EQ(test.state().send_count.load(), 2);
}

// The point of a PINGREQ is to find out whether the broker is still there.
// Without this, a session that has silently died looks connected until the next
// publish fails.
TEST(SoftwareMqttClientTest, ABrokerThatNeverAnswersAPingreqIsGivenUpOn) {
  ClientUnderTest test(std::chrono::seconds(1));
  test.client().SetKeepAlive(1);
  test.ConnectAndAccept();

  std::atomic<int> timeouts{0};
  test.client().OnError([&](const NetworkError& error) {
    if (error.code == NetworkErrc::kTimeout) ++timeouts;
  });

  ASSERT_TRUE(WaitFor([&] { return timeouts.load() > 0; },
                      std::chrono::seconds(6)));
  EXPECT_TRUE(WaitFor([&] { return !test.client().IsConnected(); },
                      std::chrono::seconds(2)));

  // No PINGRESP is staged, so the transport is closed without a DISCONNECT:
  // there is no working link to say goodbye over.
  EXPECT_EQ(test.state().disconnect_calls, 1);
}

TEST(SoftwareMqttClientTest, APingrespKeepsTheSessionAlive) {
  ClientUnderTest test;
  test.client().SetKeepAlive(1);
  test.ConnectAndAccept();

  int errors = 0;
  test.client().OnError([&](const NetworkError&) { ++errors; });

  // Answer every PINGREQ the client sends, for longer than the period the
  // client allows before giving up.
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(WaitFor([&] { return test.state().send_count.load() > i + 1; },
                        std::chrono::seconds(4)));
    ASSERT_TRUE(test.transport().Deliver(
        EncodePacket(MqttFixedHeader{MqttPacketType::kPingresp}, {})));
  }

  EXPECT_EQ(errors, 0);
  EXPECT_TRUE(test.client().IsConnected());
}

}  // namespace
