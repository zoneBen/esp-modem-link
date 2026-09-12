#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "at_channel/iat_channel.h"
#include "esp_modem_link/common_types.h"
#include "esp_modem_link/network_error.h"
#include "hal/imodule_hal.h"

namespace esp_modem_link::hal {

using DetectFn = bool (*)(at_channel::IAtChannel& channel);
using CreateFn = std::unique_ptr<IModuleHal> (*)();

struct ModuleEntry {
  ModuleType type;
  std::string_view name;
  DetectFn detect;
  CreateFn create;
};

class ModuleRegistry {
 public:
  static ModuleRegistry& Instance();

  void Register(ModuleEntry entry);

  Result<std::unique_ptr<IModuleHal>> DetectModule(
      at_channel::IAtChannel& channel);

  Result<std::unique_ptr<IModuleHal>> CreateModule(ModuleType type);

  size_t Count() const { return entries_.size(); }

 private:
  ModuleRegistry() = default;
  std::vector<ModuleEntry> entries_;
};

namespace {

struct ModuleRegistrar {
  explicit ModuleRegistrar(ModuleEntry entry) {
    ModuleRegistry::Instance().Register(std::move(entry));
  }
};

}  // namespace

#define ESP_MODEM_LINK_REGISTER_MODULE(type, name, detect_fn, create_fn) \
  static ::esp_modem_link::hal::ModuleRegistrar                          \
      _module_registrar_##type(                                          \
          {::esp_modem_link::ModuleType::type, name, detect_fn,         \
           create_fn})

}  // namespace esp_modem_link::hal
