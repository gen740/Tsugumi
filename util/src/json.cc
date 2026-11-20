#include "tsugumi/util/json.hh"

#include <stdexcept>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::schema {

namespace detail {

// CompactWriter retains the prefix up to the failed value, but hosq's encoder
// does not attach a field path to its error. Read only that prefix on failure;
// successful encodes do not pay for a second traversal or retain paths.
auto encoding_failure(std::string_view type, std::string_view partial,
                      const hosq::Error& error) -> std::string {
  struct Frame {
    std::string path;
    bool object;
    bool key = true;
    std::string member;
    size_t index = 0;
  };
  std::vector<Frame> stack;
  auto path = [&]() -> std::string {
    if (stack.empty()) return "$";
    const auto& f = stack.back();
    if (!f.object) return f.path + "[" + std::to_string(f.index) + "]";
    return f.path + (f.key ? "[key " + std::to_string(f.index) + "]"
                           : "[" + f.member + "]");
  };
  auto consumed = [&] {
    if (stack.empty()) return;
    auto& f = stack.back();
    ++f.index;
    f.key = f.object;
  };
  size_t i = 0;
  while (i < partial.size()) {
    const char c = partial[i];
    if (c == '{' || c == '[') {
      stack.push_back({path(), c == '{', true, {}, 0});
      ++i;
    } else if (c == '}' || c == ']') {
      if (stack.empty()) break;
      stack.pop_back();
      consumed();
      ++i;
    } else if (c == '"') {
      const size_t begin = i++;
      while (i < partial.size() && partial[i] != '"')
        i += partial[i] == '\\' ? 2 : 1;
      if (i >= partial.size()) break;
      ++i;
      if (!stack.empty() && stack.back().object && stack.back().key) {
        stack.back().member = partial.substr(begin, i - begin);
        stack.back().key = false;
      } else {
        consumed();
      }
    } else if (c == ',' || c == ':' || c == ' ' || c == '\n' || c == '\r' ||
               c == '\t') {
      ++i;
    } else {
      while (i < partial.size() && partial[i] != ',' && partial[i] != '}' &&
             partial[i] != ']')
        ++i;
      consumed();
    }
  }
  std::string message = "JSON encoding failed: " + std::string(type) + " at " +
                        path() + ": " + describe(error);
  std::string line;
  for (const unsigned char c : message) {
    if (c < 0x20 || c == 0x7f) {
      constexpr char hex[] = "0123456789abcdef";
      line += "\\u00";
      line += hex[c >> 4];
      line += hex[c & 15];
    } else {
      line += static_cast<char>(c);
    }
  }
  return line;
}

std::string describe(const hosq::Error& e) {
  if (e.code == hosq::ErrorCode::unknown_member && !e.path.empty() &&
      e.path.back().has_name) {
    v::String member;
    member.value() = e.path.back().name;
    std::string parent;
    for (size_t i = 0; i + 1 < e.path.size(); ++i) {
      const auto& segment = e.path[i];
      if (segment.has_name)
        parent += (parent.empty() ? "" : ".") + segment.name;
      else
        parent += "[" + std::to_string(segment.index) + "]";
    }
    return (parent.empty() ? "" : parent + ": ") + "unknown setting " +
           encode(member) + " (not in this version): remove it";
  }
  std::string msg(e.detail);
  if (msg.empty()) msg = "invalid value";
  std::string path;
  for (const auto& seg : e.path) {
    if (seg.has_name)
      path += "." + std::string(seg.name);
    else
      path += "[" + std::to_string(seg.index) + "]";
  }
  if (!path.empty()) msg = path.substr(path[0] == '.' ? 1 : 0) + ": " + msg;
  if (e.code == hosq::ErrorCode::type_mismatch && !path.empty())
    msg += "; correct this setting's value";
  return msg;
}

std::optional<DecodeError> validate(std::string_view text,
                                    hosq::json::GenericDom<>& dom) {
  auto e = dom.assign(std::string(text));
  if (e) return DecodeError{describe(e), e.offset};
  return std::nullopt;
}

}  // namespace detail

std::string text(const hosq::json::GenericDom<>& value) {
  return std::string(value[""].source());
}

auto canonical_text(const hosq::json::GenericDom<>& value) -> std::string {
  const auto raw = value[""].source();
  switch (raw.front()) {
    case '{': {
      v::Mapping<v::String, Json> object;
      if (auto error = decode(value, object))
        throw std::runtime_error(error->message);
      json::Members sorted;
      for (const auto& [key, child] : object.value())
        sorted.emplace(key, canonical_text(child));
      return json::Encode(sorted);
    }
    case '[': {
      v::Array<Json> array;
      if (auto error = decode(value, array))
        throw std::runtime_error(error->message);
      std::vector<std::string> items;
      items.reserve(array.value().size());
      for (const auto& child : array.value())
        items.push_back(canonical_text(child));
      return json::Encode(items);
    }
    case '"': {
      v::String string;
      if (auto error = decode(value, string))
        throw std::runtime_error(error->message);
      return encode(string);
    }
    default:
      return std::string(raw);
  }
}

hosq::json::GenericDom<> capture(std::string_view json_text) {
  hosq::json::GenericDom<> dom;
  if (auto e = dom.assign(std::string(json_text)))
    throw std::runtime_error("invalid JSON value: " + detail::describe(e));
  return dom;
}

hosq::json::GenericDom<> string_value(const std::string& s) {
  v::String str;
  str.value() = s;
  return capture(encode(str));
}

std::string object_text(
    const std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>>&
        members) {
  v::Mapping<v::String, Json> m;
  for (const auto& [k, val] : members) m.value().emplace_back(k, *val);
  return encode(m);
}

std::string pretty_text(std::string_view compact) {
  std::string out;
  size_t depth = 0;
  auto line = [&] {
    out += '\n';
    out.append(depth * 2, ' ');
  };
  for (size_t i = 0; i < compact.size(); ++i) {
    char c = compact[i];
    switch (c) {
      case '"': {
        size_t end = i + 1;
        while (compact[end] != '"') end += compact[end] == '\\' ? 2 : 1;
        out.append(compact, i, end - i + 1);
        i = end;
        break;
      }
      case '{':
      case '[':
        out += c;
        if (compact[i + 1] == '}' || compact[i + 1] == ']') {
          out += compact[++i];
        } else {
          ++depth;
          line();
        }
        break;
      case '}':
      case ']':
        --depth;
        line();
        out += c;
        break;
      case ',':
        out += c;
        line();
        break;
      case ':':
        out += ": ";
        break;
      default:
        out += c;
    }
  }
  out += '\n';
  return out;
}

bool is_null(const hosq::json::GenericDom<>& value) {
  v::Null n;
  return !value[""].decode_into(n);
}

}  // namespace tsugumi::schema
