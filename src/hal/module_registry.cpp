#include "hal/module_registry.h"

#include <chrono>

namespace esp_modem_link::hal {

ModuleRegistry& ModuleRegistry::Instance() {
  static ModuleRegistry instance;
  return instance;
}

void ModuleRegistry::Register(ModuleEntry entry) {
  entries_.push_back(std::move(entry));
}

Result<std::unique_ptr<IModuleHal>> ModuleRegistry::DetectModule(
    at_channel::IAtChannel& channel) {
  for (const auto& entry : entries_) {
    if (entry.detect && entry.detect(channel)) {
      auto hal = entry.create();
      if (hal) {
        return hal;
      }
    }
  }
  return std::unexpected(
      NetworkError::NotSupported("no compatible module detected"));
}

Result<std::unique_ptr<IModuleHal>> ModuleRegistry::CreateModule(
    ModuleType type) {
  for (const auto& entry : entries_) {
    if (entry.type == type && entry.create) {
      auto hal = entry.create();
      if (hal) {
        return hal;
      }
    }
  }
  return std::unexpected(
      NetworkError::NotSupported("module type not registered"));
}

}  // namespace esp_modem_link::hal
