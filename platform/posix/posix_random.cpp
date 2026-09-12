#include "platform/random.h"

#include <mutex>
#include <random>

namespace esp_modem_link::platform {
namespace {

// One generator for the process, seeded once from the system's entropy source.
// Constructing a random_device per call is far more expensive than drawing from
// it, and several masking keys over one connection are expected.
//
// std::random_device is not guaranteed to be non-deterministic, and a toolchain
// that stubs it out would leave this silently predictable. That is the case the
// note in the header is about: a platform whose generator is real should say so
// here rather than inherit the standard library's word for it.
std::mt19937& Generator() {
  static std::mt19937 generator(std::random_device{}());
  return generator;
}

}  // namespace

uint32_t RandomUint32() {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  return static_cast<uint32_t>(Generator()());
}

}  // namespace esp_modem_link::platform
