#include "strings.hh"

#include <cctype>
#include <utility>

#include "tsugumi/util/json_object.hh"

extern const unsigned char tsugumi_english_strings[];
extern const unsigned long tsugumi_english_strings_size;

namespace tsugumi::linux_ui {
namespace {

auto Current() -> Strings& {
  static Strings strings;
  return strings;
}

// A {n} with n < values.size() is replaced; anything else stays as written.
auto PutValues(std::string_view text, const std::vector<std::string>& values)
    -> std::string {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '{') {
      size_t end = i + 1;
      while (end < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[end])) != 0)
        ++end;
      if (end > i + 1 && end < text.size() && text[end] == '}') {
        const auto index =
            std::stoul(std::string(text.substr(i + 1, end - i - 1)));
        if (index < values.size()) {
          out += values[index];
          i = end + 1;
          continue;
        }
      }
    }
    out += text[i++];
  }
  return out;
}

}  // namespace

Strings::Strings(Language language, std::string_view dictionary_json)
    : language_(language) {
  if (language_ != Language::kEnglish) return;
  for (const auto& [ja, entry] : json::Object(dictionary_json)) {
    // An entry is the English text, or {text, mac} when it takes values.
    if (!entry.empty() && entry.front() == '{')
      english_.emplace(ja, json::Text(json::Object(entry), "text"));
    else
      english_.emplace(ja, json::String(entry));
  }
}

auto Strings::Text(std::string_view ja,
                   const std::vector<std::string>& values) const
    -> std::string {
  const auto found = english_.find(ja);
  const std::string_view text =
      found != english_.end() && !found->second.empty() ? found->second : ja;
  return values.empty() ? std::string(text) : PutValues(text, values);
}

auto LanguageNamed(std::string_view name) -> Language {
  return name == "en" ? Language::kEnglish : Language::kJapanese;
}

auto EnglishDictionary() -> std::string_view {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): embedded bytes
  return {reinterpret_cast<const char*>(tsugumi_english_strings),
          tsugumi_english_strings_size};
}

auto SetStrings(Strings strings) -> void { Current() = std::move(strings); }

auto L(std::string_view ja, const std::vector<std::string>& values)
    -> std::string {
  return Current().Text(ja, values);
}

}  // namespace tsugumi::linux_ui
