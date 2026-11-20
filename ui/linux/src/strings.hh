// The UI's text. The source strings are Japanese; English comes from
// ui/shared/i18n/en.json, keyed by the Japanese text, with {0}, {1}... for
// the values put in, as in the Web UI and the Mac app.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tsugumi::linux_ui {

enum class Language { kJapanese, kEnglish };

class Strings {
 public:
  Strings() = default;
  // dictionary_json: en.json's text.
  Strings(Language language, std::string_view dictionary_json);

  [[nodiscard]] auto language() const -> Language { return language_; }
  // The text in this language, with values put in for {0}, {1}...
  [[nodiscard]] auto Text(std::string_view ja,
                          const std::vector<std::string>& values = {}) const
      -> std::string;

 private:
  Language language_ = Language::kJapanese;
  std::map<std::string, std::string, std::less<>> english_;
};

// "en" and "ja" as the hostd setting app.language and TSUGUMI_LANGUAGE name
// them; anything else is Japanese.
auto LanguageNamed(std::string_view name) -> Language;

// The compiled-in en.json.
auto EnglishDictionary() -> std::string_view;

// The application's strings, set once at startup.
auto SetStrings(Strings strings) -> void;
auto L(std::string_view ja, const std::vector<std::string>& values = {})
    -> std::string;

}  // namespace tsugumi::linux_ui
