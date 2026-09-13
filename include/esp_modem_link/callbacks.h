#pragma once

#include <functional>
#include <string_view>

#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

// All of these run on the AT channel's receive task, which is also the task that
// delivers the response to every command in flight. A callback that issued a
// command - a Send from inside a DataCallback, say - would block on the command
// mutex held by the thread waiting for a response only this task can hand it,
// and the channel would stop. Hand the work to another task instead.
using DataCallback = std::function<void(std::string_view data)>;
using ErrorCallback = std::function<void(const NetworkError& error)>;
using EventCallback = std::function<void()>;

using PublishAckCallback = std::function<void(int msg_id)>;

}  // namespace esp_modem_link
