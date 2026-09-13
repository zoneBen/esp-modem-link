#pragma once

#include <chrono>
#include <string>

#include "esp_modem_link/common_types.h"

namespace esp_modem_link {

// What a TLS connection should check about the server it reaches.
//
// The defaults ask for no verification, which is what the modules this library
// targets do as shipped. A certificate reaches one of them as a file the module
// already holds - AT+MSSLCFG="cert" on the ML307 - and this library has no path
// to write one, so a HAL that is asked to verify has to be asked on hardware
// that was given a certificate some other way. The ML307R-DL-MBRH0S01 this was
// validated against holds none at all: AT+MSSLCFG="cert",0 reads back
// "NULL","NULL","NULL", and a verified handshake fails on every host while the
// same connection with auth 0 succeeds. A default of `true` there would mean a
// library whose HTTPS does not work until the caller finds that out.
//
// So the default is a connection that is encrypted but not authenticated, which
// is what the library this one was ported from does unconditionally. Asking for
// a checked server means setting both flags: the modules have one setting
// covering the chain and the hostname together, so a HAL reports it when asked
// for one without the other rather than choosing which check to keep. A HAL
// that cannot verify at all - no certificate authority to check against -
// reports the handshake failure rather than quietly connecting anyway.
struct TlsConfig {
  bool verify_certificate = false;
  bool verify_hostname = false;
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
