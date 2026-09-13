// Subscribes to an MQTT broker over the ML307 on an ESP32-C3 test board, prints
// everything that arrives on the topic, and publishes a heartbeat once a second.
//
// The module is wired to UART1 on GPIO4 (TX) / GPIO5 (RX). UART0 is left alone:
// on this chip that is the console on GPIO20/21, and the platform layer refuses
// to take over a port another driver already owns, so putting the modem there
// would only fail.
//
// The diagnostic stages print as they pass, so a failure says where it stopped
// rather than only that it did. Once subscribed the app stays up, logging each
// message and publishing its heartbeat, until it is reset.
//
//   ../../scripts/idf.ps1 -p COM7 flash monitor

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>

#include "esp_log.h"
#include "esp_modem_link/cellular_device.h"
#include "esp_modem_link/mqtt_client.h"
#include "esp_modem_link/network_interface.h"
#include "esp_modem_link/uart_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace esp_modem_link;

namespace {

constexpr const char* kTag = "c3_test";

// UART1, because UART0 is this chip's console.
constexpr int kModemUartPort = 1;
constexpr int kModemTxPin = 4;
constexpr int kModemRxPin = 5;
constexpr int kModemBaud = 115200;

// The broker to subscribe to, and the topic this board publishes to. One session
// carries both directions.
constexpr const char* kMqttHost = "cloud.mrcong.cn";
constexpr uint16_t kMqttPort = 1883;
constexpr const char* kMqttTopic = "test/";
constexpr const char* kHeartbeatTopic = "test2/";
constexpr const char* kMqttClientId = "esp32c3-test";
// Left empty for a broker that lets anonymous clients in. Fill both in together
// if it does not - the protocol requires a user name to accompany a password,
// and the client offers a credential pair as both or as neither.
constexpr const char* kMqttUser = "";
constexpr const char* kMqttPass = "";

// The data path needs a PDP context, and the APN belongs to the SIM's carrier
// rather than to this board. "cmnet" is the China Mobile one, which is what the
// ML307 in this repository has been tested against; change it for any other
// carrier or the module will never reach the network.
constexpr const char* kApn = "cmnet";

// How much of a payload to print. The console runs at 115200, so a large
// message printed in full would hold the receive task for seconds while the
// module keeps sending into a 4 KiB UART ring buffer with no flow control - and
// what overflows there is the AT line stream, not just the message. The same
// cap the sibling tool uses (tools/tcp_smoke_test.cpp).
constexpr size_t kMaxPrintedPayload = 400;

// One tick publishes one heartbeat. The period is this delay plus however long
// the publish itself takes, which is deliberate: on a link slow enough to matter
// the heartbeat thins out, instead of the loop falling behind and then firing a
// backlog of them all at once to catch up.
constexpr int kLoopTickMs = 1000;
constexpr int kAliveTicks = 30;  // 30 s

int g_failures = 0;

// Set from the receive task, acted on by the loop. A refused SUBACK arrives as
// an error rather than as a return value - Subscribe() hands back a packet id as
// soon as the packet is queued - and IsConnected() stays true through the
// refusal, so this flag is the only thing that brings the loop back to
// subscribing. Atomic because the two are different tasks.
std::atomic<bool> g_session_bad{false};

void Section(const char* name) {
  ESP_LOGI(kTag, "--- %s ---", name);
}

template <typename T>
void Show(const char* label, const Result<T>& result) {
  if (!result) {
    ESP_LOGE(kTag, "%-14s FAILED: %s (code=%d)", label, result.error().Message(),
             static_cast<int>(result.error().code));
    ++g_failures;
    return;
  }
  if constexpr (std::is_same_v<T, std::string>) {
    ESP_LOGI(kTag, "%-14s %s", label, result->c_str());
  } else if constexpr (std::is_same_v<T, int>) {
    ESP_LOGI(kTag, "%-14s %d", label, *result);
  } else {
    ESP_LOGI(kTag, "%-14s ok", label);
  }
}

const char* SimStateName(SimState state) {
  switch (state) {
    case SimState::kReady: return "READY";
    case SimState::kPinRequired: return "PIN required";
    case SimState::kPukRequired: return "PUK required";
    case SimState::kNotInserted: return "not inserted";
    case SimState::kError: return "error";
    case SimState::kUnknown: return "unknown";
  }
  return "?";
}

const char* RegStateName(RegistrationState state) {
  switch (state) {
    case RegistrationState::kNotRegistered: return "not registered";
    case RegistrationState::kRegisteredHome: return "registered (home)";
    case RegistrationState::kSearching: return "searching";
    case RegistrationState::kRegistrationDenied: return "denied";
    case RegistrationState::kRegisteredRoaming: return "registered (roaming)";
    case RegistrationState::kUnknown: return "unknown";
  }
  return "?";
}

// Stages that answer with a state rather than an error still have to be counted,
// or the run ends in a PASS nobody earned.
template <typename State>
void ShowState(const char* label, const Result<State>& result,
               const char* (*name)(State)) {
  if (result) {
    ESP_LOGI(kTag, "%-14s %s", label, name(*result));
  } else {
    ESP_LOGE(kTag, "%-14s FAILED: %s", label, result.error().Message());
    ++g_failures;
  }
}

// One connect-and-subscribe attempt. A failure after the session came up leaves
// it down on purpose: the caller's loop retries on "not connected", so a
// half-open session that never subscribed would otherwise never be revisited.
//
// What comes back here is that the request was sent, not that the broker
// accepted it - the SUBACK is answered later, and a refusal reaches OnError.
bool Subscribe(MqttClient& mqtt) {
  auto connected = mqtt.Connect(kMqttHost, kMqttPort);
  if (!connected) {
    ESP_LOGE(kTag, "connect FAILED: %s", connected.error().Message());
    return false;
  }
  ESP_LOGI(kTag, "%-14s %s:%u", "connect", kMqttHost,
           static_cast<unsigned>(kMqttPort));

  auto subscribed = mqtt.Subscribe(kMqttTopic, MqttQoS::kQoS1);
  if (!subscribed) {
    ESP_LOGE(kTag, "subscribe FAILED: %s", subscribed.error().Message());
    mqtt.Disconnect();
    return false;
  }
  ESP_LOGI(kTag, "%-14s %s (qos1, awaiting SUBACK)", "subscribe", kMqttTopic);
  return true;
}

// One heartbeat, carrying uptime so a subscriber can tell the board was reset
// rather than merely quiet.
//
// QoS 0 on purpose: nothing answers a QoS 0 publish, so it cannot stall this loop
// waiting for a PUBACK that a dead link will never send, and a heartbeat loses
// its point the moment it has to be retried. What Publish() reports is that the
// packet was queued, not that the broker has it.
void PublishHeartbeat(MqttClient& mqtt) {
  char payload[32];
  snprintf(payload, sizeof(payload), "uptime %llds",
           static_cast<long long>(esp_timer_get_time() / 1000000));

  auto published = mqtt.Publish(kHeartbeatTopic, payload, MqttQoS::kQoS0);
  if (!published) {
    ESP_LOGE(kTag, "heartbeat FAILED: %s", published.error().Message());
    // A link that died without saying so shows up here first: the socket write
    // is the only thing still touching it, and IsConnected() keeps reporting the
    // state it was last told. Raising the flag sends the loop back through the
    // one reconnect path rather than growing a second one here.
    g_session_bad.store(true);
    return;
  }
  ESP_LOGI(kTag, "heartbeat -> %s: %s", kHeartbeatTopic, payload);
}

// Brings the data path up, subscribes, and then never returns unless it could
// not get that far. On success it holds the session open for the life of the
// board, reconnecting when the cellular link drops.
void RunMqttListener(CellularDevice& device) {
  ApnConfig apn;
  apn.apn = kApn;
  auto activated = device.ConfigureApn(apn);
  if (!activated) {
    ESP_LOGE(kTag, "APN setup FAILED: %s (code=%d native=%d)",
             activated.error().Message(),
             static_cast<int>(activated.error().code),
             activated.error().native);
    ++g_failures;
    return;
  }
  ESP_LOGI(kTag, "%-14s up (APN \"%s\")", "pdp", kApn);

  // No module in this repository has an MQTT stack of its own, so this is the
  // software client over a TCP socket. Port 1883 is plaintext, which also keeps
  // this clear of TLS - this firmware ships no certificate authority, so an
  // 8883 session would fail on the certificate rather than on the broker.
  auto mqtt_result = device.GetNetwork().CreateMqtt();
  if (!mqtt_result) {
    ESP_LOGE(kTag, "CreateMqtt FAILED: %s", mqtt_result.error().Message());
    ++g_failures;
    return;
  }
  MqttClient& mqtt = **mqtt_result;

  mqtt.SetClientId(kMqttClientId);
  if (kMqttUser[0] != '\0') mqtt.SetCredentials(kMqttUser, kMqttPass);

  // This runs on the transport's receive task, so it logs and nothing else:
  // sending from here would re-enter the AT layer from inside its own dispatch.
  mqtt.OnMessage([](MqttMessage message) {
    const size_t shown = std::min(message.payload.size(), kMaxPrintedPayload);
    ESP_LOGI(kTag, "message  topic='%s' qos=%d%s %u bytes",
             message.topic.c_str(), static_cast<int>(message.qos),
             message.retain ? " retain" : "",
             static_cast<unsigned>(message.payload.size()));
    ESP_LOGI(kTag, "%.*s%s", static_cast<int>(shown), message.payload.c_str(),
             shown < message.payload.size() ? " ..." : "");
  });
  // Not "the broker closed it": this also fires when the app tears the session
  // down itself, after a subscribe that could not be sent.
  mqtt.OnDisconnected([] { ESP_LOGW(kTag, "session ended"); });
  mqtt.OnError([](const NetworkError& error) {
    ESP_LOGE(kTag, "mqtt error: %s", error.Message());
    g_session_bad.store(true);
  });

  if (Subscribe(mqtt)) {
    ESP_LOGI(kTag, "listening on '%s' at %s:%u, publishing to '%s'",
             kMqttTopic, kMqttHost, static_cast<unsigned>(kMqttPort),
             kHeartbeatTopic);
  } else {
    ++g_failures;
  }

  int ticks = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kLoopTickMs));

    // IsConnected() stays true across a refused SUBACK, so the flag is what
    // brings us back to subscribing; a dropped link is caught by the same test.
    if (!mqtt.IsConnected() || g_session_bad.exchange(false)) {
      // Reconnecting from here rather than from the callbacks keeps the send out
      // of the receive task.
      ESP_LOGW(kTag, "re-subscribing");
      if (Subscribe(mqtt)) ticks = 0;
      continue;
    }

    PublishHeartbeat(mqtt);

    if (++ticks >= kAliveTicks) {
      ticks = 0;
      // Device uptime since boot, not time in this session - it keeps counting
      // across reconnects.
      ESP_LOGI(kTag, "alive (uptime %lld s)",
               static_cast<long long>(esp_timer_get_time() / 1000000));
    }
  }
}

}  // namespace

extern "C" void app_main(void) {
  platform::UartConfig uart;
  uart.port = kModemUartPort;
  uart.tx_pin = kModemTxPin;
  uart.rx_pin = kModemRxPin;
  uart.baud_rate = kModemBaud;

  Section("create");
  // Create(), not Detect(): naming the type is the identification, so no
  // AT+CGMR goes out first. Swap in CellularDevice::Detect(uart) to have the
  // module identify itself instead.
  auto created = CellularDevice::Create(ModuleType::kMl307, uart);
  if (!created) {
    ESP_LOGE(kTag, "Create FAILED: %s (code=%d)", created.error().Message(),
             static_cast<int>(created.error().code));
    ESP_LOGE(kTag, "check TX/RX are not swapped and the module is powered");
    return;
  }
  CellularDevice& device = **created;
  ESP_LOGI(kTag, "%-14s UART%d tx=%d rx=%d at %d baud", "module", kModemUartPort,
           kModemTxPin, kModemRxPin, kModemBaud);

  Section("identity");
  Show("revision", device.GetModuleRevision());
  Show("imei", device.GetImei());
  Show("iccid", device.GetIccid());
  Show("carrier", device.GetCarrierName());

  Section("sim + network");
  ShowState("sim", device.GetSimState(), SimStateName);
  ShowState("registration", device.GetRegistrationState(), RegStateName);

  auto rssi = device.GetSignalStrength();
  if (rssi) {
    // RSSI is an index, not dBm: dBm = -113 + 2 * rssi. 99 means "no signal",
    // and would otherwise read as a very strong one.
    ESP_LOGI(kTag, "%-14s %d (%d dBm)%s", "signal", *rssi, -113 + 2 * (*rssi),
             *rssi == 99 ? " [no signal]" : "");
  } else {
    ESP_LOGE(kTag, "%-14s FAILED: %s", "signal", rssi.error().Message());
    ++g_failures;
  }

  Section("wait for registration (up to 30s)");
  ShowState("network", device.WaitForNetwork(std::chrono::seconds(30)),
            RegStateName);

  // Said here rather than at the end: the listener below does not return once it
  // is up, so a verdict printed after it would only ever be reached on failure.
  ESP_LOGI(kTag, "diagnostics: %d failure(s)", g_failures);

  Section("mqtt");
  if (kApn[0] == '\0') {
    ESP_LOGE(kTag, "kApn is empty; nothing reaches the network without it");
    ++g_failures;
  } else {
    // Does not return once it is listening.
    RunMqttListener(device);
  }

  ESP_LOGI(kTag, "%s (%d failures)", g_failures == 0 ? "PASS" : "FAIL",
           g_failures);
}
