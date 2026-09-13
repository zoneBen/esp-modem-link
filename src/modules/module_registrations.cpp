#include "modules/module_registrations.h"

#include "modules/air780e/air780e_hal.h"
#include "modules/ml307/ml307_hal.h"

namespace esp_modem_link {

void EnsureModulesRegistered() {
  // Referencing each module's force-link symbol keeps its translation unit in
  // the link, which in turn lets its static ModuleRegistrar run.
  modules::ml307::ForceLinkMl307Hal();
  modules::air780e::ForceLinkAir780eHal();

  // Phase 5 modules will be added here:
  // modules::ec801e::ForceLinkEc801eHal();
}

}  // namespace esp_modem_link
