#pragma once

#include <functional>
#include <string_view>

#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

using DataCallback = std::function<void(std::string_view data)>;
using ErrorCallback = std::function<void(const NetworkError& error)>;
using EventCallback = std::function<void()>;

using PublishAckCallback = std::function<void(int msg_id)>;

}  // namespace esp_modem_link
