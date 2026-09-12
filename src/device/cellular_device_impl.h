#pragma once

#include <memory>

#include "at_channel/iat_channel.h"
#include "hal/imodule_hal.h"
#include "hal/module_registry.h"
#include "network/cellular_network.h"
#include "platform/iuart.h"
#include "esp_modem_link/uart_config.h"

namespace esp_modem_link {

struct CellularDevice::Impl {
  // Owns the transport when this device opened one. Null when the caller
  // supplied an already-built AT channel, which it owns instead.
  std::unique_ptr<platform::IUart> uart;
  // Abstract rather than AtUart so an injected channel slots in unchanged.
  // Declared after uart_ so it is destroyed first, leaving the UART alive for
  // the whole of the channel's teardown.
  std::unique_ptr<at_channel::IAtChannel> at_uart;
  std::unique_ptr<hal::IModuleHal> hal;
  std::unique_ptr<network::CellularNetwork> network;

  Impl() = default;
};

}  // namespace esp_modem_link
