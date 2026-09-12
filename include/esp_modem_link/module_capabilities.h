#pragma once

namespace esp_modem_link {

struct ModuleCapabilities {
  bool tcp = false;
  bool udp = false;
  bool ssl_tcp = false;
  bool http = false;
  bool https = false;
  bool mqtt = false;
  bool mqtts = false;
  bool file_system = false;
  bool low_power = false;
  int max_connections = 0;
  int max_baud_rate = 115200;
};

}  // namespace esp_modem_link
