#include "platform/iuart.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace esp_modem_link::platform {

namespace {

constexpr const char* kTag = "modem_uart";

uart_word_length_t ToDataBits(int bits) {
  switch (bits) {
    case 5: return UART_DATA_5_BITS;
    case 6: return UART_DATA_6_BITS;
    case 7: return UART_DATA_7_BITS;
    default: return UART_DATA_8_BITS;
  }
}

uart_parity_t ToParity(UartParity parity) {
  switch (parity) {
    case UartParity::kEven: return UART_PARITY_EVEN;
    case UartParity::kOdd: return UART_PARITY_ODD;
    default: return UART_PARITY_DISABLE;
  }
}

uart_stop_bits_t ToStopBits(UartStopBits stop_bits) {
  return stop_bits == UartStopBits::kTwo ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

uart_hw_flowcontrol_t ToFlowControl(UartFlowControl flow) {
  switch (flow) {
    case UartFlowControl::kRts: return UART_HW_FLOWCTRL_RTS;
    case UartFlowControl::kCts: return UART_HW_FLOWCTRL_CTS;
    case UartFlowControl::kRtsCts: return UART_HW_FLOWCTRL_CTS_RTS;
    default: return UART_HW_FLOWCTRL_DISABLE;
  }
}

// Milliseconds to scheduler ticks, rounding up: a timeout shorter than a tick
// would otherwise become a zero-tick wait, which is the non-blocking form - so a
// caller asking for a few milliseconds would get an immediate 0 back and the
// loop above it would spin. Zero stays zero, because that is what the interface
// means by it.
TickType_t ToTicks(std::chrono::milliseconds timeout) {
  const int64_t ms = timeout.count();
  if (ms <= 0) return 0;
  return static_cast<TickType_t>(std::max<int64_t>(
      1, std::min<int64_t>(ms * configTICK_RATE_HZ / 1000, portMAX_DELAY - 1)));
}

class IdfUart : public IUart {
 public:
  IdfUart(uart_port_t port, const UartConfig& config)
      : port_(port), config_(config) {}

  ~IdfUart() override {
    if (installed_) uart_driver_delete(port_);
  }

  bool Install() {
    if (uart_is_driver_installed(port_)) {
      // Someone else is driving this port - the console on UART0 is the usual
      // one. Taking it over would mean undoing their configuration in this
      // object's destructor, which is not ours to undo.
      ESP_LOGE(kTag, "UART%d is already installed by another owner",
               static_cast<int>(port_));
      return false;
    }

    uart_config_t uart_config = {};
    uart_config.baud_rate = config_.baud_rate;
    uart_config.data_bits = ToDataBits(config_.data_bits);
    uart_config.parity = ToParity(config_.parity);
    uart_config.stop_bits = ToStopBits(config_.stop_bits);
    uart_config.flow_ctrl = ToFlowControl(config_.flow_control);
    // Read only when the RTS line is in play, and set below the FIFO's length so
    // the driver can ask the peer to stop before the FIFO overruns.
    uart_config.rx_flow_ctrl_thresh =
        config_.flow_control == UartFlowControl::kNone ? 0
                                                       : SOC_UART_FIFO_LEN - 8;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    if (uart_param_config(port_, &uart_config) != ESP_OK) return false;

    // -1 is this driver's "leave that pin alone", which is what an unset pin in
    // UartConfig means, so the two agree without translation. An unset TX and RX
    // is worth saying out loud: the port would come up with no pins routed and
    // nothing would ever arrive, which reads as a modem that answers nothing.
    if (config_.tx_pin < 0 && config_.rx_pin < 0) {
      ESP_LOGW(kTag, "UART%d has no TX/RX pins configured; none will be routed",
               static_cast<int>(port_));
    }
    if (uart_set_pin(port_, config_.tx_pin, config_.rx_pin, config_.rts_pin,
                     config_.cts_pin) != ESP_OK) {
      return false;
    }

    // The driver's ring buffer has to be larger than the hardware FIFO, and a
    // caller's 4 KiB default is well past that; a smaller one is raised rather
    // than refused.
    const int rx_buffer = static_cast<int>(
        std::max<size_t>(config_.rx_buffer_size, SOC_UART_FIFO_LEN * 2));
    if (uart_driver_install(port_, rx_buffer,
                            static_cast<int>(config_.tx_buffer_size), 0,
                            nullptr, 0) != ESP_OK) {
      return false;
    }
    installed_ = true;
    return true;
  }

  void SetBaudRate(int baud) override {
    config_.baud_rate = baud;
    // The driver waits for what is already in the FIFO to go out before it
    // changes the divisor, which is what this has to do: the module on the other
    // end is being told to change rate by a command that is still in flight.
    if (installed_) uart_set_baudrate(port_, baud);
  }

  int GetBaudRate() const override {
    if (installed_) {
      // uart_get_baudrate wants a uint32_t, not an int like the interface uses.
      // The read-back only ever produces what a previous SetBaudRate asked for,
      // so it fits either way.
      uint32_t baud = 0;
      if (uart_get_baudrate(port_, &baud) == ESP_OK) {
        return static_cast<int>(baud);
      }
    }
    return config_.baud_rate;
  }

  int Send(const void* data, size_t len) override {
    if (!installed_) return -1;
    // Blocks until the bytes are in the ring buffer rather than on the wire,
    // which is what the interface promises: a caller treating a short return as
    // a lost write must not see one from a busy transmitter.
    return uart_write_bytes(port_, data, len);
  }

  int Receive(void* buffer,
              size_t len,
              std::chrono::milliseconds timeout) override {
    if (!installed_) return 0;
    return uart_read_bytes(port_, buffer, static_cast<uint32_t>(len),
                           ToTicks(timeout));
  }

  void Flush() override {
    if (!installed_) return;
    // Both directions, because the host implementation purges both and what has
    // already been handed to the transmitter is the closest thing to something
    // that can still be taken back. Bounded rather than portMAX_DELAY: with CTS
    // flow control on and nothing asserting CTS, the FIFO would never drain and
    // an unbounded wait would hang the caller instead of flushing.
    uart_wait_tx_done(port_, pdMS_TO_TICKS(1000));
    uart_flush_input(port_);
  }

 private:
  uart_port_t port_;
  UartConfig config_;
  bool installed_ = false;
};

}  // namespace

std::unique_ptr<IUart> CreateUart(const UartConfig& config) {
  if (config.port < 0 || config.port >= static_cast<int>(UART_NUM_MAX)) {
    ESP_LOGE(kTag, "this chip has no UART%d", config.port);
    return nullptr;
  }

  // UartConfig::device is host-only - a COM port or a /dev path - so it is
  // ignored here rather than reported: on this platform the port number and the
  // pins are the whole of what names a UART.
  auto uart =
      std::make_unique<IdfUart>(static_cast<uart_port_t>(config.port), config);
  if (!uart->Install()) return nullptr;
  return uart;
}

}  // namespace esp_modem_link::platform
