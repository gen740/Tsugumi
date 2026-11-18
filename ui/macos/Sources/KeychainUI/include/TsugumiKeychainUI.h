#pragma once
#include <stdbool.h>

// Call only while holding Keychain.gate; this switch is process-wide.
void TsugumiSetKeychainInteractionAllowed(bool allowed);
