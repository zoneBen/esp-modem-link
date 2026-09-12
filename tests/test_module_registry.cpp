#include <gtest/gtest.h>

#include <string>

#include "at_channel/iat_channel.h"
#include "hal/module_registry.h"
#include "modules/ml307/ml307_hal.h"

using namespace esp_modem_link;
using namespace esp_modem_link::hal;
using esp_modem_link::at_channel::IAtChannel;

namespace {

bool DetectFakeModule(IAtChannel& channel) {
  auto r = channel.SendCommand("AT", std::chrono::milliseconds(100));
  if (!r) return false;
  auto lines = channel.GetResponseLines();
  for (const auto& line : lines) {
    if (line.find("FAKE_MODULE") != std::string_view::npos) return true;
  }
  return false;
}

std::unique_ptr<IModuleHal> CreateFakeModule() {
  return nullptr;
}

}  // namespace

TEST(ModuleRegistryTest, RegisterAndCount) {
  // Force ml307_hal.obj to be linked so its static registrar runs
  modules::ml307::ForceLinkMl307Hal();

  auto& reg = ModuleRegistry::Instance();
  size_t count = reg.Count();
  EXPECT_GE(count, 1u);  // At least ML307
}

TEST(ModuleRegistryTest, Ml307IsRegistered) {
  auto& reg = ModuleRegistry::Instance();
  auto result = reg.CreateModule(ModuleType::kMl307);
  EXPECT_TRUE(result.has_value());
  EXPECT_NE(result.value(), nullptr);
  if (result.has_value()) {
    EXPECT_EQ(result.value()->GetModuleType(), ModuleType::kMl307);
    EXPECT_EQ(std::string(result.value()->GetModuleName()), "ML307");
  }
}

TEST(ModuleRegistryTest, CreateUnknownModuleFails) {
  auto& reg = ModuleRegistry::Instance();
  auto result = reg.CreateModule(ModuleType::kUnknown);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

TEST(ModuleRegistryTest, Ml307Capabilities) {
  auto& reg = ModuleRegistry::Instance();
  auto result = reg.CreateModule(ModuleType::kMl307);
  ASSERT_TRUE(result.has_value());
  const auto& caps = result.value()->GetCapabilities();
  EXPECT_TRUE(caps.tcp);
  EXPECT_TRUE(caps.udp);
  EXPECT_TRUE(caps.ssl_tcp);
  EXPECT_TRUE(caps.http);
  EXPECT_TRUE(caps.https);
  EXPECT_TRUE(caps.mqtt);
  EXPECT_TRUE(caps.mqtts);
  EXPECT_EQ(caps.max_connections, 6);
  EXPECT_EQ(caps.max_baud_rate, 921600);
}
