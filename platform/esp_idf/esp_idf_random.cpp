#include "platform/random.h"

#include "esp_random.h"

namespace esp_modem_link::platform {

// Backed by the chip's generator rather than a seeded PRNG, which is the case
// random.h asks a platform to be honest about: the one caller is the WebSocket
// masking key, and a key an observer can predict is the whole of what the
// masking exists to prevent. esp_random() draws on the RF entropy source and is
// safe to call from several tasks, so there is nothing to guard here.
uint32_t RandomUint32() { return esp_random(); }

}  // namespace esp_modem_link::platform
