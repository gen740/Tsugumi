#include "nix_params.hh"

#include <algorithm>
#include <charconv>
#include <cmath>
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
#include <set>
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

using Positions = std::set<std::pair<uint32_t, uint32_t>>;

auto position(std::string_view before) -> std::pair<uint32_t, uint32_t> {
  const auto line = std::ranges::count(before, '\n') + 1;
  const auto last = before.rfind('\n');
  const auto column =
      last == std::string_view::npos ? before.size() + 1 : before.size() - last;
  return {line, column};
}

// Nix requires a decimal point before an exponent. Also retain two
// restrictions the AST cannot express: the parser folds literal dynamic keys
// to symbols and lowers unary and binary subtraction to the same builtin.
auto prepare(std::string_view text, const std::string& origin,
             Positions& minimum_integers) -> std::string {
  if (text.find('\0') != std::string_view::npos)
    throw std::runtime_error(origin + ": NUL byte in params.nix");
  constexpr auto interpolation =
      "$"
      "{";
  std::string out;
  char previous = '\0';
  size_t minus_offset = 0;
  size_t cursor = 0;
  const auto error = [&](std::string_view message) {
    const auto [line, column] = position(text.substr(0, cursor));
    return std::runtime_error(origin + ":" + std::to_string(line) + ":" +
                              std::to_string(column) + ": " +
                              std::string(message));
  };
  const auto copy_until = [&](size_t end) {
    out.append(text.substr(cursor, end - cursor));
    cursor = end;
  };
  while (cursor < text.size()) {
    const auto tail = text.substr(cursor);
    const char token = tail.front();
    if (token == '#') {
      const auto end = text.find('\n', cursor);
      copy_until(end == std::string_view::npos ? text.size() : end);
    } else if (tail.starts_with("/*")) {
      const auto end = text.find("*/", cursor + 2);
      if (end == std::string_view::npos) throw error("unterminated comment");
      copy_until(end + 2);
    } else if (token == '"' || tail.starts_with("''")) {
      const bool indented = token != '"';
      copy_until(cursor + (indented ? 2 : 1));
      for (;;) {
        if (cursor == text.size()) throw error("unterminated string");
        const auto rest = text.substr(cursor);
        if (rest.starts_with(interpolation))
          throw error("interpolation is not allowed");
        if (!indented && rest.starts_with("\\") && rest.size() > 1) {
          copy_until(cursor + 2);
        } else if (indented &&
                   (rest.starts_with("'''") || rest.starts_with("''$") ||
                    rest.starts_with("''\\"))) {
          const size_t escape = rest.starts_with("''\\") ? 4 : 3;
          copy_until(cursor + std::min(escape, rest.size()));
        } else if (rest.starts_with(indented ? "''" : "\"")) {
          copy_until(cursor + (indented ? 2 : 1));
          break;
        } else {
          copy_until(cursor + 1);
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
      copy_until(end);
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
        const size_t exponent = end++;
        if (end < text.size() && (text[end] == '-' || text[end] == '+')) ++end;
        const size_t first_digit = end;
        while (end < text.size() && digit(text[end])) ++end;
        if (!decimal && end > first_digit) {
          copy_until(exponent);
          out += ".0";
        }
      }
      const bool minimum =
          previous == '-' &&
          text.substr(cursor, end - cursor) == "9223372036854775808";
      copy_until(end);
      if (minimum) {
        // Nix tokenizes unsigned digits first. Record where to adjust the
        // verified unary subtraction so the evaluated result is INT64_MIN.
        minimum_integers.insert(
            position(std::string_view(out).substr(0, minus_offset)));
        out.back() = '7';
      }
      previous = 'v';
    } else {
      if (token == ':' || tail.starts_with(interpolation))
        throw error("expected a literal without interpolation");
      if (token == '-' && previous != '=' && previous != '(' &&
          previous != '[' && previous != '\0')
        throw error("only unary minus on a number literal is allowed");
      if (token == '-') minus_offset = out.size();
      if (std::string_view(" \t\r\n").find(token) == std::string_view::npos)
        previous = token;
      copy_until(cursor + 1);
    }
  }
  return out;
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
                 const std::string& where, Positions& minimum_integers)
    -> std::string {
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
      auto* zero = dynamic_cast<nix::ExprInt*>(call->args->at(0));
      const auto* number = call->args->at(1);
      if (zero && zero->v.integer().value == 0) {
        if (dynamic_cast<const nix::ExprInt*>(number)) {
          if (!minimum_integers.empty()) {
            const auto pos = state.positions[call->pos];
            if (minimum_integers.erase({pos.line, pos.column}))
              zero->v.mkInt(-1);
          }
          return "int";
        }
        if (dynamic_cast<const nix::ExprFloat*>(number)) return "float";
      }
    }
  }
  fail(where, "expected a bool, int, float or string literal");
}

auto declaration_type(nix::EvalState& state, const nix::Expr* expr,
                      const std::string& where, Positions& minimum_integers)
    -> std::string {
  const auto* list = dynamic_cast<const nix::ExprList*>(expr);
  if (!list) return scalar_type(state, expr, where, minimum_integers);
  if (list->elems.empty()) fail(where, "empty lists are not allowed");
  const auto type =
      scalar_type(state, list->elems.front(), where, minimum_integers);
  for (const auto* element : list->elems.subspan(1))
    if (scalar_type(state, element, where, minimum_integers) != type)
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
}  // namespace

auto read_params_nix(nix::EvalState& state, std::string_view text,
                     const std::string& origin) -> std::string {
  try {
    Positions minimum_integers;
    auto prepared = prepare(text, origin, minimum_integers);
    auto* expr =
        state.parseExprFromString(std::move(prepared), state.rootPath("/"));
    using ParamTypes =
        std::map<std::string, std::map<std::string, std::string>>;
    ParamTypes types{{"run", {}}, {"build", {}}};
    std::set<std::string> names;
    for (const auto& [section, attr] : *plain_attrs(expr, origin).attrs) {
      const std::string key(state.symbols[section]);
      if (!types.contains(key)) fail(origin, "unknown top-level key " + key);
      const auto where = origin + ": " + key;
      for (const auto& [symbol, item] : *plain_attrs(attr.e, where).attrs) {
        const std::string name(state.symbols[symbol]);
        if (!valid_id(name))
          fail(where, "invalid param name " + json::Quote(name));
        if (!names.insert(name).second) fail(where, "duplicate param " + name);
        types[key].emplace(name,
                           declaration_type(state, item.e, where + "." + name,
                                            minimum_integers));
      }
    }
    if (!minimum_integers.empty()) fail(origin, "invalid int64 literal");
    // Check the complete AST before evaluating any thunk.
    nix::Value value;
    state.eval(expr, value);
    const auto section_json = [&](const std::string& key) {
      std::vector<std::string> items;
      if (const auto* attr = value.attrs()->get(state.symbols.create(key))) {
        state.forceValue(*attr->value, nix::noPos);
        for (const auto& [name, type] : types.at(key)) {
          auto* param =
              attr->value->attrs()->get(state.symbols.create(name))->value;
          items.push_back("{\"name\":" + json::Quote(name) +
                          ",\"type\":" + json::Quote(type) +
                          ",\"default\":" + value_json(state, *param) + "}");
        }
      }
      return json::Encode(items);
    };
    return "{\"run\":" + section_json("run") +
           ",\"build\":" + section_json("build") + "}";
  } catch (const nix::BaseError& error) {
    throw std::runtime_error(origin + ": " +
                             nix::filterANSIEscapes(error.what(), true));
  }
}
// Prepared runtimes and standalone stores can parse literals without an
// evaluator subprocess.
auto evaluate_literal_params(const std::string& def_dir, std::string_view text)
    -> std::string {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  static const bool initialized = [] {
    nix::initLibStore(false);
    nix::initGC();
    return true;
  }();
  (void)initialized;
  static std::map<std::string, std::string> cache;
  const auto hash = crypto::sha256_hex(text);
  if (const auto entry = cache.find(hash); entry != cache.end())
    return entry->second;
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
  auto result = read_params_nix(state, text, def_dir + "params.nix");
  if (cache.size() >= 256) cache.erase(cache.begin());
  cache.emplace(hash, result);
  return result;
}
}  // namespace tsugumi
