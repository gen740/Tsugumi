#include <Security/Security.h>

#include "TsugumiKeychainUI.h"

void TsugumiSetKeychainInteractionAllowed(bool allowed) {
  // SecItem.h: no-authentication query attributes apply to the Data Protection
  // keychain; legacy login keychain items may still display UI. Changing stores
  // would change credential access. Keep this call visible as the sole warning,
  // without allowing any other deprecated API in our Swift or C targets.
#pragma clang diagnostic push
#pragma clang diagnostic warning "-Wdeprecated-declarations"
  SecKeychainSetUserInteractionAllowed(allowed);
#pragma clang diagnostic pop
}
