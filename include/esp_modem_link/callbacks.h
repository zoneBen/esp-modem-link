#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

using DataCallback = std::function<void(std::string_view data)>;
using ErrorCallback = std::function<void(const NetworkError& error)>;
using EventCallback = std::function<void()>;

using MessageCallback =
    std::function<void(std::string_view topic, std::string_view payload)>;

using PublishAckCallback = std::function<void(int msg_id)>;

}  // namespace esp_modem_link
