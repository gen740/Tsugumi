// Salted, deliberately slow password hashes (scrypt) for a local
// administrator password. The stored form never contains the password.
#pragma once

#include <string>
#include <string_view>

namespace tsugumi::password {

// "scrypt$<log2 N>$<r>$<p>$<salt hex>$<key hex>" with a fresh random salt.
auto Hash(std::string_view password) -> std::string;
// Whether `password` matches a stored hash, compared in constant time.
// A malformed stored hash matches nothing.
auto Verify(std::string_view stored, std::string_view password) -> bool;

}  // namespace tsugumi::password
