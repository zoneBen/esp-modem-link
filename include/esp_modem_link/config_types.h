#pragma once

#include <chrono>
#include <string>

#include "esp_modem_link/common_types.h"

namespace esp_modem_link {

struct TlsConfig {
  bool verify_certificate = true;
  bool verify_hostname = true;
  std::string ca_cert;
  std::string client_cert;
  std::string client_key;
  std::string alpn_protocols;
  std::chrono::seconds handshake_timeout{10};
};

struct MqttWill {
  std::string topic;
  std::string payload;
  int qos = 0;
  bool retain = false;
};

struct ApnConfig {
  std::string apn;
  std::string username;
  std::string password;
};

struct SleepConfig {
  bool enable_dtr_wakeup = true;
  std::chrono::seconds inactivity_timeout{30};
};

}  // namespace esp_modem_link
