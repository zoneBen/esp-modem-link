#include "platform/igpio.h"

namespace esp_modem_link::platform {

namespace {

class PosixGpio : public IGpio {
 public:
  explicit PosixGpio(int pin) : pin_(pin) {}

  void SetLevel(GpioLevel level) override { level_ = level; }
  GpioLevel GetLevel() const override { return level_; }
  void SetDirection(bool output) override { output_ = output; }
  void EnableInterrupt(GpioEdge edge,
                        GpioInterruptHandler handler) override {
    edge_ = edge;
    handler_ = std::move(handler);
  }
  void DisableInterrupt() override { handler_ = nullptr; }

 private:
  int pin_;
  GpioLevel level_ = GpioLevel::kLow;
  bool output_ = false;
  GpioEdge edge_ = GpioEdge::kRising;
  GpioInterruptHandler handler_;
};

}  // namespace

std::unique_ptr<IGpio> CreateGpio(int pin) {
  return std::make_unique<PosixGpio>(pin);
}

}  // namespace esp_modem_link::platform
