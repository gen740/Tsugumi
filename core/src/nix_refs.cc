#include "nix_refs.hh"

#include <cctype>

namespace tsugumi {

namespace {

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'' || c == '-'; }
bool space(char c) { return std::isspace(static_cast<unsigned char>(c)); }

}  // namespace

std::vector<std::pair<bool, std::string>> nix_references(const std::string& t) {
  std::vector<std::pair<bool, std::string>> out;
  enum class Mode { Code, Str, Ind };
  // What each nesting level is; in code, how many `{` are open in it.
  struct Level {
    Mode mode;
    int braces;
  };
  std::vector<Level> stack = {{Mode::Code, 0}};
  auto starts = [&](size_t at, const char* s) { return t.compare(at, std::char_traits<char>::length(s), s) == 0; };
  size_t i = 0;
  while (i < t.size()) {
    Level& top = stack.back();
    char c = t[i];
    if (top.mode == Mode::Str) {
      if (c == '\\') {
        i += 2;
      } else if (c == '"') {
        stack.pop_back();
        ++i;
      } else if (starts(i, "${")) {
        stack.push_back({Mode::Code, 0});
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (top.mode == Mode::Ind) {
      if (starts(i, "''$") || starts(i, "'''")) {
        i += 3;
      } else if (starts(i, "''\\")) {
        i += 4;
      } else if (starts(i, "''")) {
        stack.pop_back();
        i += 2;
      } else if (starts(i, "${")) {
        stack.push_back({Mode::Code, 0});
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    // Code.
    if (c == '#') {
      while (i < t.size() && t[i] != '\n') ++i;
    } else if (starts(i, "/*")) {
      size_t e = t.find("*/", i + 2);
      i = e == std::string::npos ? t.size() : e + 2;
    } else if (c == '"') {
      stack.push_back({Mode::Str, 0});
      ++i;
    } else if (starts(i, "''")) {
      stack.push_back({Mode::Ind, 0});
      i += 2;
    } else if (c == '{') {
      ++top.braces;
      ++i;
    } else if (c == '}') {
      // The end of an interpolation goes back to its string.
      if (top.braces == 0 && stack.size() > 1) stack.pop_back();
      else --top.braces;
      ++i;
    } else if (ident_start(c) && (i == 0 || !ident_char(t[i - 1]))) {
      size_t e = i;
      while (e < t.size() && ident_char(t[e])) ++e;
      std::string word = t.substr(i, e - i);
      size_t at = i;
      i = e;
      if (word != "upstream" && word != "local") continue;
      // Not an attribute of something else, except of `inputs`.
      size_t b = at;
      while (b > 0 && space(t[b - 1])) --b;
      if (b > 0 && t[b - 1] == '.') {
        size_t we = b - 1;
        while (we > 0 && space(t[we - 1])) --we;
        size_t ws = we;
        while (ws > 0 && ident_char(t[ws - 1])) --ws;
        if (t.substr(ws, we - ws) != "inputs") continue;
      }
      size_t p = e;
      while (p < t.size() && space(t[p])) ++p;
      if (p >= t.size() || t[p] != '.') continue;
      ++p;
      while (p < t.size() && space(t[p])) ++p;
      std::string name;
      if (p < t.size() && t[p] == '"') {
        size_t q = p + 1;
        while (q < t.size() && t[q] != '"' && t[q] != '\\' && t[q] != '$') name += t[q++];
        if (q >= t.size() || t[q] != '"') continue;  // an escape or interpolation: not a plain name
        i = q + 1;
      } else if (p < t.size() && ident_start(t[p])) {
        size_t q = p;
        while (q < t.size() && ident_char(t[q])) ++q;
        name = t.substr(p, q - p);
        i = q;
      } else {
        continue;
      }
      out.emplace_back(word == "local", name);
    } else {
      ++i;
    }
  }
  return out;
}

}  // namespace tsugumi
