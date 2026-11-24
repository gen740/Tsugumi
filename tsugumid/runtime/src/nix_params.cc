#include "nix_params.hh"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <nix/expr/eval-inline.hh>
#include <nix/expr/eval-settings.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/terminal.hh>
#include <stdexcept>
#include <vector>

#include "tsugumi/document/document.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi {
namespace {

auto identifier(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
auto digit(char c) -> bool { return c >= '0' && c <= '9'; }

struct Token {
  size_t begin;
  size_t end;
  bool comment;
};

auto position(std::string_view before) -> std::pair<uint32_t, uint32_t> {
  const auto line = std::ranges::count(before, '\n') + 1;
  const auto last = before.rfind('\n');
  const auto column =
      last == std::string_view::npos ? before.size() + 1 : before.size() - last;
  return {line, column};
}

// Check restrictions hidden by Nix's AST: the parser can fold literal
// dynamic keys to symbols and lower subtraction to the same builtin.
auto scan_literals(std::string_view text, const std::string& origin,
                   std::vector<Token>* tokens = nullptr) -> void {
  if (text.find('\0') != std::string_view::npos)
    throw std::runtime_error(origin + ": NUL byte in params.nix");
  constexpr auto interpolation =
      "$"
      "{";
  char previous = '\0';
  size_t cursor = 0;
  const auto error = [&](std::string_view message) {
    const auto [line, column] = position(text.substr(0, cursor));
    return std::runtime_error(origin + ":" + std::to_string(line) + ":" +
                              std::to_string(column) + ": " +
                              std::string(message));
  };
  const auto advance = [&](size_t end) { cursor = end; };
  while (cursor < text.size()) {
    const auto tail = text.substr(cursor);
    const char token = tail.front();
    const auto begin = cursor;
    const bool comment = token == '#' || tail.starts_with("/*");
    if (token == '#') {
      const auto end = text.find('\n', cursor);
      advance(end == std::string_view::npos ? text.size() : end);
    } else if (tail.starts_with("/*")) {
      const auto end = text.find("*/", cursor + 2);
      if (end == std::string_view::npos) throw error("unterminated comment");
      advance(end + 2);
    } else if (token == '"' || tail.starts_with("''")) {
      const bool indented = token != '"';
      advance(cursor + (indented ? 2 : 1));
      for (;;) {
        if (cursor == text.size()) throw error("unterminated string");
        const auto rest = text.substr(cursor);
        if (rest.starts_with(interpolation))
          throw error("interpolation is not allowed");
        if (!indented && rest.starts_with("\\") && rest.size() > 1) {
          advance(cursor + 2);
        } else if (indented &&
                   (rest.starts_with("'''") || rest.starts_with("''$") ||
                    rest.starts_with("''\\"))) {
          const size_t escape = rest.starts_with("''\\") ? 4 : 3;
          advance(cursor + std::min(escape, rest.size()));
        } else if (rest.starts_with(indented ? "''" : "\"")) {
          advance(cursor + (indented ? 2 : 1));
          break;
        } else {
          advance(cursor + 1);
        }
      }
      previous = '"';
    } else if (identifier(token)) {
      size_t end = cursor + 1;
      while (end < text.size() && (identifier(text[end]) || digit(text[end]) ||
                                   text[end] == '-' || text[end] == '\''))
        ++end;
      if (text.substr(cursor, end - cursor) == "inherit")
        throw error("inherit is not allowed");
      advance(end);
      previous = 'v';
    } else if (digit(token) ||
               (token == '.' && tail.size() > 1 && digit(tail[1]))) {
      size_t end = cursor;
      while (end < text.size() && digit(text[end])) ++end;
      const bool decimal = end < text.size() && text[end] == '.';
      if (decimal) {
        ++end;
        while (end < text.size() && digit(text[end])) ++end;
      }
      if (end < text.size() && (text[end] == 'e' || text[end] == 'E')) {
        ++end;
        if (end < text.size() && (text[end] == '-' || text[end] == '+')) ++end;
        while (end < text.size() && digit(text[end])) ++end;
      }
      advance(end);
      previous = 'v';
    } else {
      if (token == ':' || tail.starts_with(interpolation))
        throw error("expected a literal without interpolation");
      if (token == '-' && previous != '=' && previous != '(' &&
          previous != '[' && previous != '\0')
        throw error("only unary minus on a number literal is allowed");
      if (std::string_view(" \t\r\n").find(token) == std::string_view::npos)
        previous = token;
      advance(cursor + 1);
    }
    if (tokens &&
        std::string_view(" \t\r\n").find(token) == std::string_view::npos)
      tokens->push_back({begin, cursor, comment});
  }
}

[[noreturn]] auto fail(const std::string& where, std::string_view message)
    -> void {
  throw std::runtime_error(where + ": " + std::string(message));
}

auto plain_attrs(nix::Expr* expression, const std::string& where)
    -> const nix::ExprAttrs& {
  const auto* set = dynamic_cast<nix::ExprAttrs*>(expression);
  if (!set || set->recursive || !set->pos || !set->dynamicAttrs->empty())
    fail(where, "expected a plain literal attribute set");
  for (const auto& [_, attr] : *set->attrs)
    if (attr.kind != nix::ExprAttrs::AttrDef::Kind::Plain)
      fail(where, "inherit is not allowed");
  return *set;
}

auto scalar_type(nix::EvalState& state, const nix::Expr* expr,
                 const std::string& where) -> std::string {
  if (dynamic_cast<const nix::ExprInt*>(expr)) return "int";
  if (dynamic_cast<const nix::ExprFloat*>(expr)) return "float";
  if (dynamic_cast<const nix::ExprString*>(expr)) return "string";
  if (const auto* var = dynamic_cast<const nix::ExprVar*>(expr)) {
    const auto name = state.symbols[var->name];
    if (name == "true" || name == "false") return "bool";
  }
  if (const auto* call = dynamic_cast<const nix::ExprCall*>(expr)) {
    const auto* var = dynamic_cast<const nix::ExprVar*>(call->fun);
    if (var && !var->pos && state.symbols[var->name] == "__sub" &&
        call->args->size() == 2) {
      const auto* zero = dynamic_cast<const nix::ExprInt*>(call->args->at(0));
      const auto* number = call->args->at(1);
      if (zero && zero->v.integer().value == 0) {
        if (dynamic_cast<const nix::ExprInt*>(number)) return "int";
        if (dynamic_cast<const nix::ExprFloat*>(number)) return "float";
      }
    }
  }
  fail(where, "expected a bool, int, float or string literal");
}

auto literal_type(nix::EvalState& state, const nix::Expr* expr,
                  const std::string& where) -> std::string {
  const auto* list = dynamic_cast<const nix::ExprList*>(expr);
  if (!list) return scalar_type(state, expr, where);
  if (list->elems.empty()) fail(where, "empty lists are not allowed");
  const auto type = scalar_type(state, list->elems.front(), where);
  for (const auto* element : list->elems.subspan(1))
    if (scalar_type(state, element, where) != type)
      fail(where, "list elements must have the same type");
  return "[" + type + "]";
}

auto value_json(nix::EvalState& state, nix::Value& value) -> std::string {
  state.forceValue(value, nix::noPos);
  switch (value.type()) {
    case nix::nBool:
      return value.boolean() ? "true" : "false";
    case nix::nInt:
      return std::to_string(value.integer().value);
    case nix::nFloat: {
      char buffer[64];
      const double number = value.fpoint();
      if (!std::isfinite(number)) throw std::runtime_error("non-finite float");
      const auto [end, error] =
          std::to_chars(buffer, buffer + sizeof buffer, number);
      if (error != std::errc()) throw std::runtime_error("cannot print float");
      std::string out(buffer, end);
      if (out.find_first_of(".eE") == std::string::npos) out += ".0";
      return out;
    }
    case nix::nString:
      return json::Quote(value.string_view());
    case nix::nList: {
      std::vector<std::string> items;
      for (auto* item : value.listView())
        items.push_back(value_json(state, *item));
      return json::Encode(items);
    }
    default:
      throw std::logic_error("literal parameter has an unexpected Nix value");
  }
}

auto read_params_nix(nix::EvalState& state, std::string_view text,
                     const std::string& origin) -> std::string {
  try {
    if (text.size() > kMaxDocumentJsonBytes)
      fail(origin, "exceeds the 8 MiB document limit");
    scan_literals(text, origin);
    auto* expr =
        state.parseExprFromString(std::string(text), state.rootPath("/"));
    std::map<std::string, std::string> types;
    for (const auto& [symbol, item] : *plain_attrs(expr, origin).attrs) {
      const std::string name(state.symbols[symbol]);
      if (!valid_id(name))
        fail(origin, "invalid param name " + json::Quote(name));
      types.emplace(name, literal_type(state, item.e, origin + ": " + name));
    }
    // Check the complete AST before evaluating any thunk.
    nix::Value value;
    state.eval(expr, value);
    std::vector<std::string> items;
    for (const auto& [name, type] : types) {
      auto* param = value.attrs()->get(state.symbols.create(name))->value;
      items.push_back("{\"name\":" + json::Quote(name) +
                      ",\"type\":" + json::Quote(type) +
                      ",\"value\":" + value_json(state, *param) + "}");
    }
    auto result = json::Encode(items);
    if (result.size() > kMaxDocumentJsonBytes)
      fail(origin, "params.nix values exceed 8 MiB");
    return result;
  } catch (const nix::BaseError& error) {
    throw std::runtime_error(origin + ": " +
                             nix::filterANSIEscapes(error.what(), true));
  }
}

// Parameter literals do not need project files or a real Nix store.
auto with_literal_state(
    const std::function<std::string(nix::EvalState&)>& evaluate)
    -> std::string {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  static const bool initialized = [] {
    nix::initLibStore(false);
    nix::initGC();
    return true;
  }();
  (void)initialized;
  bool read_only = true;
  nix::EvalSettings settings(read_only);
  settings.pureEval = true;
  settings.restrictEval = true;
  settings.enableImportFromDerivation = false;
  settings.nixPath = nix::Strings{};
  settings.allowedUris = nix::Strings{};
  nix::fetchers::Settings fetch_settings;
  nix::EvalState state(nix::LookupPath{}, nix::openStore("dummy://"),
                       fetch_settings, settings);
  return evaluate(state);
}
}  // namespace

auto evaluate_literal_params(const std::string& def_dir, std::string_view text)
    -> std::string {
  static std::mutex mutex;
  static std::map<std::string, std::string> cache;
  std::lock_guard lock(mutex);
  const auto hash = crypto::sha256_hex(text);
  if (const auto entry = cache.find(hash); entry != cache.end())
    return entry->second;
  auto result = with_literal_state([&](nix::EvalState& state) {
    return read_params_nix(state, text, def_dir + "params.nix");
  });
  if (cache.size() >= 256) cache.erase(cache.begin());
  cache.emplace(hash, result);
  return result;
}

auto rewrite_param_literal(const std::string& path, std::string_view text,
                           std::string_view name, std::string_view literal)
    -> std::string {
  return with_literal_state([&](nix::EvalState& state) {
    const auto declaration = read_params_nix(state, text, path);
    std::string type;
    for (const auto& raw : json::Array(declaration)) {
      const auto entry = json::Object(raw);
      if (json::Text(entry, "name") == name) type = json::Text(entry, "type");
    }
    if (type.empty()) fail(path, "undeclared parameter " + std::string(name));

    constexpr std::string_view prefix = "{ value = ";
    const auto candidate = std::string(prefix) + std::string(literal) + "\n; }";
    std::vector<Token> replacement;
    scan_literals(candidate, path, &replacement);
    // Parse the replacement as one value, independently of the original file.
    const auto parsed = json::Array(read_params_nix(state, candidate, path));
    if (parsed.size() != 1 ||
        json::Text(json::Object(parsed.front()), "type") != type)
      fail(path, "parameter " + std::string(name) + " expects " + type);
    const auto in_literal = [&](const Token& token) {
      return token.begin >= prefix.size() &&
             token.end <= prefix.size() + literal.size() && !token.comment;
    };
    const auto first = std::ranges::find_if(replacement, in_literal);
    const auto last =
        std::find_if(replacement.rbegin(), replacement.rend(), in_literal);
    if (first == replacement.end()) fail(path, "expected a parameter literal");
    const auto value =
        literal.substr(first->begin - prefix.size(), last->end - first->begin);

    std::vector<Token> tokens;
    scan_literals(text, path, &tokens);
    const auto spelling = [&](const Token& t) {
      return text.substr(t.begin, t.end - t.begin);
    };
    for (auto eq = tokens.begin(); eq != tokens.end(); ++eq) {
      if (spelling(*eq) != "=") continue;
      auto key = std::prev(eq);
      while (key->comment) --key;
      auto key_text = spelling(*key);
      if (key_text.starts_with('"')) {
        const auto* string =
            dynamic_cast<const nix::ExprString*>(state.parseExprFromString(
                std::string(key_text), state.rootPath("/")));
        if (!string || string->v.string_view() != name) continue;
      } else if (key_text != name) {
        continue;
      }
      auto start = std::next(eq);
      while (start->comment) ++start;
      auto semicolon = std::find_if(start, tokens.end(), [&](const Token& t) {
        return spelling(t) == ";";
      });
      auto end = std::prev(semicolon);
      while (end->comment) --end;
      std::string result(text.substr(0, start->begin));
      result += value;
      if (std::any_of(start, end, [](const Token& t) { return t.comment; })) {
        // A replacement list can have a different number of elements. Keep
        // the old comments and their whitespace in order after the literal.
        auto cursor = start->end;
        for (auto t = std::next(start); t <= end; ++t) {
          if (t->comment) continue;
          result += text.substr(cursor, t->begin - cursor);
          cursor = t->end;
        }
      }
      result += text.substr(end->end);
      return result;
    }
    throw std::logic_error("cannot locate verified parameter literal");
  });
}
}  // namespace tsugumi
