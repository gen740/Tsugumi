#include "host/save.hh"

namespace tsugumi::host {
auto CardStem(const std::string &name) -> std::string {
  std::string stem;
  for (unsigned char c : name) {
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
      stem += static_cast<char>(c);
    else if (!stem.empty() && stem.back() != '-')
      stem += '-';
    if (stem.size() == 123)
      break;
  }
  while (!stem.empty() && stem.back() == '-')
    stem.pop_back();
  return stem.empty() ? "card" : stem;
}

} // namespace tsugumi::host
