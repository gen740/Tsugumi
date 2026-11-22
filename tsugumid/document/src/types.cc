#include "tsugumi/document/types.hh"

#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>

#include <algorithm>
#include <set>
#include <sstream>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/flatbuffer_schema.hh"

namespace tsugumi::types {

namespace {

using reflection::BaseType;

flatbuffers::IDLOptions idl_options() {
  flatbuffers::IDLOptions o;
  o.binary_schema_builtins = true;
  o.strict_json = true;
  o.natural_utf8 = true;
  o.output_default_scalars_in_json = true;
  o.indent_step = -1;
  return o;
}

std::unique_ptr<flatbuffers::Parser> make_parser() {
  auto p = std::make_unique<flatbuffers::Parser>(idl_options());
  p->known_attributes_["tsugumi"] = true;
  return p;
}

auto serialize_schema(flatbuffers::Parser& parser) -> void {
  struct Restore {
    std::vector<std::pair<std::string*, std::string>> values;
    ~Restore() {
      for (auto& [target, value] : values) *target = std::move(value);
    }
  } restore;
  // The reflection format stores unsigned defaults in signed 64-bit slots.
  // FlatBuffers' serializer parses their text as signed, losing values above
  // INT64_MAX unless their bit representation is supplied explicitly.
  for (auto* object : parser.structs_.vec)
    for (auto* field : object->fields.vec) {
      if (field->value.type.base_type != flatbuffers::BASE_TYPE_ULONG ||
          field->IsOptional())
        continue;
      uint64_t value = 0;
      if (flatbuffers::StringToNumber(field->value.constant.c_str(), &value) &&
          value > INT64_MAX) {
        restore.values.emplace_back(&field->value.constant,
                                    field->value.constant);
        field->value.constant = std::to_string(static_cast<int64_t>(value));
      }
    }
  parser.Serialize();
}

std::string serialize_with_root(flatbuffers::Parser& parser,
                                const std::string& root, std::string* error) {
  if (!parser.SetRootType(root.c_str())) {
    *error = "unknown root type " + root;
    return {};
  }
  serialize_schema(parser);
  return std::string(
      reinterpret_cast<const char*>(parser.builder_.GetBufferPointer()),
      parser.builder_.GetSize());
}

const char* scalar_name(BaseType t) {
  switch (t) {
    case BaseType::Bool:
      return "bool";
    case BaseType::Byte:
      return "byte";
    case BaseType::UByte:
      return "ubyte";
    case BaseType::Short:
      return "short";
    case BaseType::UShort:
      return "ushort";
    case BaseType::Int:
      return "int";
    case BaseType::UInt:
      return "uint";
    case BaseType::Long:
      return "long";
    case BaseType::ULong:
      return "ulong";
    case BaseType::Float:
      return "float";
    case BaseType::Double:
      return "double";
    case BaseType::String:
      return "string";
    default:
      return nullptr;
  }
}

bool is_float(BaseType t) {
  return t == BaseType::Float || t == BaseType::Double;
}

// Renders reachable declarations of a schema under a namespace prefix.
class Renderer {
 public:
  // `nested`: the closure becomes a member of a merged type. A member that is
  // itself a merged type has its own tsugumi.merge namespaces renamed to
  // "nested", so no dotted name of one level can be read as another level's
  // (FlatBuffers also tries shorter namespace prefixes when it resolves a
  // name). Member types that are not merged keep their names.
  Renderer(const reflection::Schema& s, std::string prefix, bool nested = false)
      : s_(s), prefix_(std::move(prefix)), nested_(nested) {}

  std::string original(const std::string& full) const {
    static const std::string merge = "tsugumi.merge";
    if (nested_ && (full == merge || full.rfind(merge + ".", 0) == 0))
      return "nested" + full.substr(merge.size());
    return full;
  }

  std::string qualified(const std::string& name) const {
    return prefix_.empty() ? original(name) : prefix_ + "." + original(name);
  }

  void object(int index) {
    if (!objs_.insert(index).second) return;
    const auto* o = s_.objects()->Get(index);
    for (const auto* f : *o->fields()) {
      deps(*f->type());
      if (const auto* nested = nested_root(*f))
        for (flatbuffers::uoffset_t i = 0; i < s_.objects()->size(); ++i)
          if (s_.objects()->Get(i) == nested) object(static_cast<int>(i));
    }
    order_.push_back({true, index});
  }

  void enumeration(int index) {
    if (!enums_.insert(index).second) return;
    const auto* e = s_.enums()->Get(index);
    if (e->is_union())
      for (const auto* v : *e->values())
        if (v->union_type() && v->union_type()->index() >= 0)
          object(v->union_type()->index());
    order_.push_back({false, index});
  }

  void emit(std::string& out) {
    for (auto [is_obj, index] : order_) {
      std::string full =
          original(is_obj ? s_.objects()->Get(index)->name()->str()
                          : s_.enums()->Get(index)->name()->str());
      auto dot = full.rfind('.');
      std::string ns = dot == std::string::npos ? "" : full.substr(0, dot);
      std::string base = dot == std::string::npos ? full : full.substr(dot + 1);
      std::string qns =
          prefix_.empty() ? ns : (ns.empty() ? prefix_ : prefix_ + "." + ns);
      out += "namespace " + qns + ";\n";
      if (is_obj)
        emit_object(*s_.objects()->Get(index), base, out);
      else
        emit_enum(*s_.enums()->Get(index), base, out);
    }
  }

 private:
  auto nested_root(const reflection::Field& field) const
      -> const reflection::Object* {
    if (const auto* attrs = field.attributes())
      if (const auto* nested = attrs->LookupByKey("nested_flatbuffer"))
        return s_.objects()->LookupByKey(nested->value()->c_str());
    return nullptr;
  }

  void deps(const reflection::Type& t) {
    BaseType ref = t.base_type();
    if (ref == BaseType::Vector || ref == BaseType::Array ||
        ref == BaseType::Vector64)
      ref = t.element();
    if (t.index() < 0) return;
    if (ref == BaseType::Obj)
      object(t.index());
    else
      enumeration(t.index());
  }

  std::string type_name(const reflection::Type& t, BaseType bt) {
    if (bt == BaseType::Obj)
      return qualified(s_.objects()->Get(t.index())->name()->str());
    if (t.index() >= 0 && bt != BaseType::String)
      return qualified(s_.enums()->Get(t.index())->name()->str());
    const char* n = scalar_name(bt);
    return n ? n : "ubyte";
  }

  void emit_object(const reflection::Object& o, const std::string& name,
                   std::string& out) {
    out += o.is_struct() ? "struct " : "table ";
    out += name + " {\n";
    std::vector<const reflection::Field*> fields(o.fields()->begin(),
                                                 o.fields()->end());
    std::sort(fields.begin(), fields.end(),
              [](auto* a, auto* b) { return a->id() < b->id(); });
    for (const auto* f : fields) {
      const auto& t = *f->type();
      if (t.base_type() == BaseType::UType)
        continue;  // implicit union type field
      std::string tn;
      if (t.base_type() == BaseType::Vector ||
          t.base_type() == BaseType::Vector64)
        tn = "[" + type_name(t, t.element()) + "]";
      else if (t.base_type() == BaseType::Array)
        tn = "[" + type_name(t, t.element()) + ":" +
             std::to_string(t.fixed_length()) + "]";
      else
        tn = type_name(t, t.base_type());
      out += "  " + f->name()->str() + ":" + tn;
      BaseType bt = t.base_type();
      bool scalar = bt >= BaseType::Bool && bt <= BaseType::Double;
      if (scalar && !o.is_struct()) {
        if (f->optional()) {
          out += " = null";
        } else if (is_float(bt)) {
          std::ostringstream ss;
          ss.precision(17);
          ss << f->default_real();
          out += " = " + ss.str();
        } else if (bt == BaseType::Bool) {
          out += f->default_integer() ? " = true" : " = false";
        } else if (bt == BaseType::ULong) {
          out += " = " +
                 std::to_string(static_cast<uint64_t>(f->default_integer()));
        } else {
          out += " = " + std::to_string(f->default_integer());
        }
      }
      std::vector<std::string> attrs;
      if (!o.is_struct()) {
        // Union value fields occupy two ids; the declared id is the value one.
        attrs.push_back("id: " + std::to_string(f->id()));
      }
      if (f->required()) attrs.push_back("required");
      if (f->deprecated()) attrs.push_back("deprecated");
      if (const auto* nested = nested_root(*f))
        attrs.push_back("nested_flatbuffer: \"" +
                        qualified(nested->name()->str()) + "\"");
      if (!attrs.empty()) {
        out += " (";
        for (size_t i = 0; i < attrs.size(); ++i)
          out += (i ? ", " : "") + attrs[i];
        out += ")";
      }
      out += ";\n";
    }
    out += "}\n";
  }

  void emit_enum(const reflection::Enum& e, const std::string& name,
                 std::string& out) {
    if (e.is_union()) {
      out += "union " + name + " { ";
      bool first = true;
      for (const auto* v : *e.values()) {
        if (v->value() == 0) continue;
        if (!first) out += ", ";
        first = false;
        out += v->name()->str() + ": " +
               qualified(
                   s_.objects()->Get(v->union_type()->index())->name()->str()) +
               " = " + std::to_string(v->value());
      }
      out += " }\n";
      return;
    }
    out += "enum " + name + " : " +
           scalar_name(e.underlying_type()->base_type()) + " { ";
    bool first = true;
    for (const auto* v : *e.values()) {
      if (!first) out += ", ";
      first = false;
      out += v->name()->str() + " = " + std::to_string(v->value());
    }
    out += " }\n";
  }

  const reflection::Schema& s_;
  std::string prefix_;
  bool nested_ = false;
  std::set<int> objs_;
  std::set<int> enums_;
  std::vector<std::pair<bool, int>> order_;
};

TypeRef make_builtin(const char* text, const char* root) {
  std::string err;
  auto t = compile(text, root, &err);
  if (!t) throw std::logic_error("builtin type failed: " + err);
  return *t;
}

}  // namespace

const reflection::Schema* Type::schema() const {
  return reflection::GetSchema(bfbs.data());
}

const reflection::Object* Type::root() const { return schema()->root_table(); }

std::optional<TypeRef> compile(const std::string& fbs_text,
                               const std::string& root, std::string* error) {
  auto parser = make_parser();
  if (!parser->Parse(fbs_text.c_str(), nullptr, "schema.fbs")) {
    *error = parser->error_;
    return std::nullopt;
  }
  auto bfbs = serialize_with_root(*parser, root, error);
  if (bfbs.empty()) return std::nullopt;
  auto t = std::make_shared<Type>();
  t->bfbs = std::move(bfbs);
  t->hash = crypto::sha256_hex(t->bfbs);
  t->root_type = t->root() ? t->root()->name()->str() : root;
  if (!t->root() || t->root()->is_struct()) {
    *error = "root " + root + " must be a table";
    return std::nullopt;
  }
  return t;
}

std::optional<std::string> schema_bfbs(const std::string& fbs_text,
                                       std::string* root, std::string* error) {
  auto parser = make_parser();
  if (!parser->Parse(fbs_text.c_str(), nullptr, "_outputs.fbs")) {
    *error = parser->error_;
    return std::nullopt;
  }
  if (parser->root_struct_def_) {
    const auto* sd = parser->root_struct_def_;
    *root = sd->defined_namespace->GetFullyQualifiedName(sd->name);
  }
  serialize_schema(*parser);
  return std::string(
      reinterpret_cast<const char*>(parser->builder_.GetBufferPointer()),
      parser->builder_.GetSize());
}

namespace {

// Positions of the tokens of a schema text (identifiers and single
// punctuation), skipping comments and string literals.
struct Tok {
  size_t begin;
  size_t end;
  char punct;  // 0 for an identifier or literal
};

std::vector<Tok> tokens(const std::string& s) {
  std::vector<Tok> out;
  for (size_t i = 0; i < s.size();) {
    char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      size_t e = s.find("*/", i + 2);
      i = e == std::string::npos ? s.size() : e + 2;
      continue;
    }
    if (c == '"') {
      size_t j = i + 1;
      while (j < s.size() && s[j] != '"') j += s[j] == '\\' ? 2 : 1;
      out.push_back({i, std::min(j + 1, s.size()), 0});
      i = j + 1;
      continue;
    }
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
      size_t j = i;
      while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) ||
                              s[j] == '_' || s[j] == '.'))
        ++j;
      out.push_back({i, j, 0});
      i = j;
      continue;
    }
    out.push_back({i, i + 1, c});
    ++i;
  }
  return out;
}

}  // namespace

std::optional<std::string> set_catalog_port(const std::string& fbs_text,
                                            const std::string& port,
                                            const std::string& type_name,
                                            PortKind kind, std::string* error) {
  auto parser = make_parser();
  if (!parser->Parse(fbs_text.c_str(), nullptr, "_outputs.fbs")) {
    *error = parser->error_;
    return std::nullopt;
  }
  const auto* catalog = parser->root_struct_def_;
  if (!catalog) {
    *error = "root_type (the port catalog) is required";
    return std::nullopt;
  }
  std::string written = "string";
  if (kind != PortKind::Path) {
    // Use the short name for a type in the catalog's namespace.
    const flatbuffers::StructDef* type = nullptr;
    for (const auto* sd : parser->structs_.vec) {
      auto full = sd->defined_namespace->GetFullyQualifiedName(sd->name);
      if (!sd->fixed && sd != catalog &&
          (full == type_name || sd->name == type_name)) {
        type = sd;
        if (full == type_name) break;
      }
    }
    if (!type) {
      *error = "no table " + type_name + " in the schema";
      return std::nullopt;
    }
    written = type->defined_namespace == catalog->defined_namespace
                  ? type->name
                  : type->defined_namespace->GetFullyQualifiedName(type->name);
  }
  // The catalog's body, then the port's declaration in it.
  const auto& s = fbs_text;
  auto t = tokens(s);
  auto text_of = [&](const Tok& k) {
    return s.substr(k.begin, k.end - k.begin);
  };
  size_t body = 0;
  size_t body_end = 0;
  for (size_t i = 0; i + 2 < t.size(); ++i) {
    if (!t[i].punct && text_of(t[i]) == "table" && !t[i + 1].punct &&
        text_of(t[i + 1]) == catalog->name) {
      size_t j = i + 2;
      while (j < t.size() && t[j].punct != '{') ++j;  // attributes of the table
      body = j + 1;
      int depth = 1;
      for (size_t k = body; k < t.size(); ++k) {
        if (t[k].punct == '{') ++depth;
        if (t[k].punct == '}' && --depth == 0) {
          body_end = k;
          break;
        }
      }
      break;
    }
  }
  if (!body_end) {
    *error = "table " + catalog->name + " not found";
    return std::nullopt;
  }
  size_t field = 0;
  size_t semi = 0;
  bool at_start = true;
  for (size_t i = body; i < body_end; ++i) {
    if (at_start && !t[i].punct && text_of(t[i]) == port && i + 1 < body_end &&
        t[i + 1].punct == ':') {
      field = i;
      for (size_t k = i; k < body_end; ++k)
        if (t[k].punct == ';') {
          semi = k;
          break;
        }
      break;
    }
    at_start = t[i].punct == ';';
  }
  if (!semi) {
    *error = "port " + port + " not found in table " + catalog->name;
    return std::nullopt;
  }
  // Attributes kept as written, except tsugumi.
  std::vector<std::string> attrs;
  for (size_t i = field; i < semi; ++i) {
    if (t[i].punct != '(') continue;
    size_t start = i + 1;
    for (size_t k = start; k < semi; ++k) {
      if (t[k].punct == ',' || t[k].punct == ')') {
        if (k > start) {
          auto item = s.substr(t[start].begin, t[k - 1].end - t[start].begin);
          if (text_of(t[start]) != "tsugumi") attrs.push_back(item);
        }
        start = k + 1;
        if (t[k].punct == ')') break;
      }
    }
    break;
  }
  if (kind == PortKind::Path) attrs.push_back("tsugumi: \"path\"");
  std::string decl = port + ":" + written;
  if (!attrs.empty()) {
    decl += " (";
    for (size_t i = 0; i < attrs.size(); ++i)
      decl += (i ? ", " : "") + attrs[i];
    decl += ")";
  }
  std::string out =
      s.substr(0, t[field].begin) + decl + s.substr(t[semi].begin);
  // The tsugumi attribute needs its declaration.
  if (kind != PortKind::FlatBuffer &&
      out.find("attribute \"tsugumi\"") == std::string::npos)
    out += "attribute \"tsugumi\";\n";
  auto check = parse_catalog(out, "_outputs.fbs");
  if (!check.errors.empty()) {
    *error = check.errors.front();
    return std::nullopt;
  }
  return out;
}

CatalogResult parse_catalog(const std::string& fbs_text,
                            const std::string& filename) {
  CatalogResult r;
  auto parser = make_parser();
  if (!parser->Parse(fbs_text.c_str(), nullptr, filename.c_str())) {
    r.errors.push_back(parser->error_);
    return r;
  }
  if (!parser->root_struct_def_) {
    r.errors.push_back(filename + ": root_type (the port catalog) is required");
    return r;
  }
  const auto* catalog = parser->root_struct_def_;
  if (catalog->fixed) {
    r.errors.push_back(filename + ": the catalog root must be a table");
    return r;
  }
  for (const auto* field : catalog->fields.vec) {
    if (field->value.type.base_type == flatbuffers::BASE_TYPE_UTYPE) continue;
    PortType port;
    port.name = field->name;
    port.required = field->IsRequired();
    if (const auto* attr = field->attributes.Lookup("tsugumi")) {
      if (attr->constant != "path") {
        r.errors.push_back(filename + ": unknown tsugumi attribute \"" +
                           attr->constant + "\"");
      } else if (field->value.type.base_type != flatbuffers::BASE_TYPE_STRING) {
        r.errors.push_back(filename + ": Path port " + field->name +
                           " must have type string");
      } else {
        port.kind = PortKind::Path;
        r.ports.push_back(std::move(port));
        continue;
      }
    }
    if (field->value.type.base_type != flatbuffers::BASE_TYPE_STRUCT ||
        field->value.type.struct_def->fixed) {
      r.errors.push_back(filename + ": port " + field->name +
                         " must be a table type");
      r.ports.push_back(std::move(port));
      continue;
    }
    std::string err;
    const auto* sd = field->value.type.struct_def;
    auto bfbs = serialize_with_root(
        *parser, sd->defined_namespace->GetFullyQualifiedName(sd->name), &err);
    if (bfbs.empty()) {
      r.errors.push_back(filename + ": " + err);
    } else {
      auto t = std::make_shared<Type>();
      t->bfbs = std::move(bfbs);
      t->hash = crypto::sha256_hex(t->bfbs);
      t->root_type = t->root()->name()->str();
      port.type = t;
    }
    r.ports.push_back(std::move(port));
  }
  return r;
}

std::string render_closure(const Type& t, const std::string& prefix,
                           std::string& out, bool nested) {
  const auto* s = t.schema();
  int root = -1;
  for (flatbuffers::uoffset_t i = 0; i < s->objects()->size(); ++i)
    if (s->objects()->Get(i) == s->root_table()) root = static_cast<int>(i);
  Renderer r(*s, prefix, nested);
  r.object(root);
  r.emit(out);
  return r.qualified(s->root_table()->name()->str());
}

auto card_catalog(const std::vector<PortType>& ports, bool inputs)
    -> std::string {
  const std::string side = inputs ? "inputs" : "outputs";
  const std::string table = inputs ? "Inputs" : "Outputs";
  std::string types = "attribute \"tsugumi\";\n";
  std::string catalog = "namespace " + side + ";\ntable " + table + " {\n";
  int id = 0;
  for (const auto& port : ports) {
    if (!port.resolved()) continue;
    const bool path = port.kind == PortKind::Path;
    const auto root =
        path ? "string"
             : render_closure(*port.type, side + "." + port.name, types);
    catalog += "  " + port.name + ":" + root + " (id: " + std::to_string(id++) +
               (path ? ", tsugumi: \"path\"" : "") +
               (!inputs && port.required ? ", required" : "") + ");\n";
  }
  return types + catalog + "}\nroot_type " + table + ";\n";
}

TypeRef empty_type() {
  static TypeRef t = make_builtin("namespace tsugumi.builtin; table Empty {}",
                                  "tsugumi.builtin.Empty");
  return t;
}

auto merged_type(const std::vector<const PortType*>& inputs, std::string* error)
    -> std::optional<MergeType> {
  if (inputs.empty()) {
    *error = "a Merge needs at least one connected input";
    return std::nullopt;
  }
  if (std::ranges::any_of(inputs, [](const auto* port) {
        return port->kind == PortKind::Path;
      })) {
    if (!std::ranges::all_of(inputs, [](const auto* port) {
          return port->kind == PortKind::Path;
        })) {
      *error = "Path and FlatBuffer inputs cannot share a Merge output";
      return std::nullopt;
    }
    return MergeType{};
  }
  std::map<std::string, size_t> seen;
  std::vector<const PortType*> members;
  MergeType result;
  for (const auto* port : inputs) {
    std::string closure;
    const auto root = render_closure(*port->type, "type", closure);
    const auto [entry, inserted] =
        seen.try_emplace(closure + root, members.size());
    if (inserted) members.push_back(port);
    if (members.size() > UINT8_MAX) {
      *error = "a Merge supports at most 255 distinct FlatBuffer types";
      return std::nullopt;
    }
    result.tags.emplace(port->name, static_cast<uint8_t>(entry->second + 1));
  }
  if (members.size() == 1)
    return MergeType{
        members.front()->type, {}, members.front()->merge_member_types};

  std::string text;
  std::string boxes = "namespace tsugumi.merge;\n";
  std::string arrival = "union Arrival { ";
  std::set<std::string> names;
  std::map<std::string, size_t> short_name_counts;
  auto short_name = [](const std::string& full) {
    return full.substr(full.find_last_of('.') + 1);
  };
  for (const auto* member : members)
    ++short_name_counts[short_name(member->type->root_type)];
  for (size_t i = 0; i < members.size(); ++i) {
    const auto& member = *members[i];
    const auto& full = member.type->root_type;
    const auto short_type = short_name(full);
    const auto base = short_type + "Box";
    std::string box = base;
    for (size_t suffix = 2; !names.insert(box).second; ++suffix)
      box = base + std::to_string(suffix);
    const auto root =
        render_closure(*member.type, "tsugumi.merge.t" + std::to_string(i),
                       text, /*nested=*/true);
    boxes += "table " + box + " { bytes:[ubyte] (id: 0, nested_flatbuffer: \"" +
             root + "\"); }\n";
    arrival += (i ? ", " : "") + box;
    if (member.merge_member_types.empty()) {
      result.member_types.push_back(
          short_name_counts.at(short_type) > 1 ? full : short_type);
    } else {
      result.member_types.insert(result.member_types.end(),
                                 member.merge_member_types.begin(),
                                 member.merge_member_types.end());
    }
  }
  text += boxes + arrival + " }\ntable Merged { value:Arrival (id: 1); }\n";
  auto type = compile(text, "tsugumi.merge.Merged", error);
  if (!type) return std::nullopt;
  result.type = *type;
  return result;
}

namespace {

std::unique_ptr<flatbuffers::Parser> parser_from_type(const Type& t,
                                                      std::string* error) {
  auto p = make_parser();
  if (!p->Deserialize(reinterpret_cast<const uint8_t*>(t.bfbs.data()),
                      t.bfbs.size())) {
    *error = "cannot load schema: " + p->error_;
    return nullptr;
  }
  if (!p->SetRootType(t.root_type.c_str())) {
    *error = "unknown root type " + t.root_type;
    return nullptr;
  }
  flatbuffer_schema::restore_reflection_fields(*p, *t.schema(),
                                               /*restore_deprecated=*/true);
  return p;
}

}  // namespace

auto wrap_in_union(uint8_t tag, const std::string& bytes) -> std::string {
  flatbuffers::FlatBufferBuilder b(bytes.size() + 64);
  b.ForceVectorAlignment(bytes.size(), sizeof(uint8_t),
                         FLATBUFFERS_MAX_ALIGNMENT);
  auto nested = b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                               bytes.size());
  auto start = b.StartTable();
  b.AddOffset(flatbuffers::FieldIndexToOffset(0), nested);  // Box.bytes, id 0
  auto box = flatbuffers::Offset<flatbuffers::Table>(b.EndTable(start));
  start = b.StartTable();
  b.AddElement<uint8_t>(flatbuffers::FieldIndexToOffset(0), tag,
                        0);  // Merged.value_type, id 0
  b.AddOffset(flatbuffers::FieldIndexToOffset(1), box);  // Merged.value, id 1
  b.Finish(flatbuffers::Offset<flatbuffers::Table>(b.EndTable(start)));
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}

bool verify(const Type& t, const std::string& bytes) {
  return flatbuffers::Verify(*t.schema(), *t.root(),
                             reinterpret_cast<const uint8_t*>(bytes.data()),
                             bytes.size());
}

bool json_to_flatbuffer(const Type& t, const std::string& json,
                        std::string* out, std::string* error) {
  auto p = parser_from_type(t, error);
  if (!p) return false;
  if (!p->ParseJson(json.c_str())) {
    *error = p->error_;
    return false;
  }
  out->assign(reinterpret_cast<const char*>(p->builder_.GetBufferPointer()),
              p->builder_.GetSize());
  return true;
}

std::string params_schema(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string text = "namespace parameters;\ntable Params {\n";
  int id = 0;
  for (const auto& [name, type] : fields) {
    bool scalar =
        type == "bool" || type == "long" || type == "ulong" || type == "double";
    text += "  " + name + ":" + type + (scalar ? " = null" : "") +
            " (id: " + std::to_string(id++) + ");\n";
  }
  text += "}\nroot_type Params;\n";
  return text;
}

}  // namespace tsugumi::types
