#include "tsugumi/document/types.hh"

#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>

#include <algorithm>
#include <set>
#include <sstream>

#include "tsugumi/document/input_schema.hh"
#include "tsugumi/document/schema_id.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"
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
      if (const auto* nested = field->nested_flatbuffer) {
        auto* attribute = field->attributes.Lookup("nested_flatbuffer");
        restore.values.emplace_back(&attribute->constant, attribute->constant);
        attribute->constant =
            nested->defined_namespace->GetFullyQualifiedName(nested->name);
      }
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
  Renderer(const reflection::Schema& s, std::string prefix,
           InputDeclarations* declarations = nullptr)
      : s_(s), prefix_(std::move(prefix)), declarations_(declarations) {}

  std::string qualified(const std::string& name) const {
    if (declarations_ && artifact(name)) return name;
    return prefix_.empty() ? name : prefix_ + "." + name;
  }

  void object(int index) {
    if (!objs_.insert(index).second) return;
    const auto* o = s_.objects()->Get(index);
    if (declarations_ && artifact(o->name()->string_view())) {
      declarations_->artifact = true;
      return;
    }
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
          is_obj ? s_.objects()->Get(index)->name()->str()
                 : s_.enums()->Get(index)->name()->str();
      auto dot = full.rfind('.');
      std::string ns = dot == std::string::npos ? "" : full.substr(0, dot);
      std::string base = dot == std::string::npos ? full : full.substr(dot + 1);
      std::string qns =
          prefix_.empty() ? ns : (ns.empty() ? prefix_ : prefix_ + "." + ns);
      std::string text;
      if (!qns.empty()) text += "namespace " + qns + ";\n";
      if (is_obj)
        emit_object(*s_.objects()->Get(index), base, text);
      else
        emit_enum(*s_.enums()->Get(index), base, text);
      if (declarations_) {
        const auto name = qualified(full);
        auto [entry, added] = declarations_->types.try_emplace(name);
        if (added) {
          entry->second.text = std::move(text);
          declarations_->order.push_back(name);
        } else if (entry->second.text != text) {
          throw std::runtime_error("conflicting input schema declarations for " + name);
        }
      } else {
        out += text;
      }
    }
  }

 private:
  static auto artifact(std::string_view name) -> bool {
    return name == "tsugumi.Artifact" || name == "tsugumi.ArtifactId";
  }

  using Attributes = flatbuffers::Vector<flatbuffers::Offset<reflection::KeyValue>>;
  auto copied_attributes(const Attributes* attributes,
                         std::string_view handled = "") -> std::vector<std::string> {
    std::vector<std::string> items;
    if (!declarations_ || !attributes) return items;
    static const flatbuffers::Parser parser;
    for (const auto* attr : *attributes) {
      const auto key = attr->key()->str();
      if (key == "tsugumi_schema" ||
          handled.find(" " + key + " ") != std::string_view::npos)
        continue;
      const bool known = parser.known_attributes_.contains(key);
      if (!known) declarations_->attributes.insert(key);
      const auto value = attr->value() ? attr->value()->str() : "";
      if (value.empty() || (known && value == "0"))
        items.push_back(key);
      else if (known)
        items.push_back(key + ": " + value);
      else
        items.push_back(key + ": " + json::Quote(value));
    }
    return items;
  }

  static auto emit_attributes(const std::vector<std::string>& attrs,
                              std::string& out) -> void {
    if (attrs.empty()) return;
    out += " (";
    for (size_t i = 0; i < attrs.size(); ++i)
      out += (i ? ", " : "") + attrs[i];
    out += ")";
  }

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
    out += name;
    emit_attributes(copied_attributes(o.attributes()), out);
    out += " {\n";
    std::vector<const reflection::Field*> fields(o.fields()->begin(),
                                                 o.fields()->end());
    std::sort(fields.begin(), fields.end(),
              [](auto* a, auto* b) { return a->id() < b->id(); });
    for (const auto* f : fields) {
      const auto& t = *f->type();
      if (t.base_type() == BaseType::UType || t.element() == BaseType::UType)
        continue;  // implicit union discriminator, including vectors of unions
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
      auto attrs = copied_attributes(f->attributes(), " id required deprecated key nested_flatbuffer ");
      if (!o.is_struct()) {
        // Union value fields occupy two ids; the declared id is the value one.
        attrs.push_back("id: " + std::to_string(f->id()));
      }
      if (f->required()) attrs.push_back("required");
      if (f->deprecated()) attrs.push_back("deprecated");
      if (declarations_ && f->key()) attrs.push_back("key");
      if (const auto* nested = nested_root(*f))
        attrs.push_back("nested_flatbuffer: \"" +
                        qualified(nested->name()->str()) + "\"");
      emit_attributes(attrs, out);
      out += ";\n";
    }
    out += "}\n";
  }

  void emit_enum(const reflection::Enum& e, const std::string& name,
                 std::string& out) {
    if (e.is_union()) {
      out += "union " + name;
      emit_attributes(copied_attributes(e.attributes()), out);
      out += " { ";
      bool first = true;
      for (const auto* v : *e.values()) {
        if (v->value() == 0) continue;
        if (!first) out += ", ";
        first = false;
        out += v->name()->str() + ": " +
               type_name(*v->union_type(), v->union_type()->base_type()) +
               " = " + std::to_string(v->value());
        emit_attributes(copied_attributes(v->attributes()), out);
      }
      out += " }\n";
      return;
    }
    out += "enum " + name + " : " +
           scalar_name(e.underlying_type()->base_type());
    // Values in reflection already use their wire bit masks.
    emit_attributes(copied_attributes(e.attributes(), " bit_flags "), out);
    out += " { ";
    bool first = true;
    for (const auto* v : *e.values()) {
      if (!first) out += ", ";
      first = false;
      out += v->name()->str() + " = " + std::to_string(v->value());
      emit_attributes(copied_attributes(v->attributes()), out);
    }
    out += " }\n";
  }

  const reflection::Schema& s_;
  std::string prefix_;
  InputDeclarations* declarations_;
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
  if (!parse_schema(*parser, fbs_text, "schema.fbs", {})) {
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
  if (!parse_schema(*parser, fbs_text, "_outputs.fbs", {})) {
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

struct CatalogFieldDecl {
  std::string name;
  size_t field;
  size_t semi;
};

auto catalog_fields(std::string_view text, const std::vector<Tok>& tok,
                    const flatbuffers::StructDef& catalog)
    -> std::vector<CatalogFieldDecl> {
  const auto full =
      catalog.defined_namespace->GetFullyQualifiedName(catalog.name);
  const auto dot = full.rfind('.');
  const auto catalog_namespace = dot == std::string::npos
                                     ? std::string_view{}
                                     : std::string_view(full).substr(0, dot);
  const auto value = [&](size_t i) {
    return text.substr(tok[i].begin, tok[i].end - tok[i].begin);
  };
  std::string current_namespace;
  size_t depth = 0;
  for (size_t i = 0; i + 2 < tok.size(); ++i) {
    if (tok[i].punct == '{') ++depth;
    if (tok[i].punct == '}') --depth;
    if (depth != 0) continue;
    if (value(i) == "namespace") {
      current_namespace.clear();
      while (i + 1 < tok.size() && tok[++i].punct != ';')
        current_namespace += value(i);
      continue;
    }
    if (value(i) != "table" || value(i + 1) != catalog.name ||
        current_namespace != catalog_namespace)
      continue;
    size_t body = i + 2;
    while (body < tok.size() && tok[body].punct != '{') ++body;
    std::vector<CatalogFieldDecl> fields;
    for (size_t field = body + 1;
         field < tok.size() && tok[field].punct != '}';) {
      size_t semi = field + 1;
      while (semi < tok.size() && tok[semi].punct != ';') ++semi;
      if (semi == tok.size()) break;
      fields.push_back({std::string(value(field)), field, semi});
      field = semi + 1;
    }
    return fields;
  }
  return {};
}

}  // namespace

std::optional<std::string> set_catalog_port(const std::string& fbs_text,
                                            const std::string& port,
                                            const std::string& type_name,
                                            PortKind kind, std::string* error) {
  auto parser = make_parser();
  if (!parse_schema(*parser, fbs_text, "_outputs.fbs", {})) {
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
  const auto& s = fbs_text;
  const auto t = tokens(s);
  const auto fields = catalog_fields(s, t, *catalog);
  const auto declaration =
      std::ranges::find(fields, port, &CatalogFieldDecl::name);
  if (declaration == fields.end()) {
    *error = "port " + port + " not found in table " + catalog->name;
    return std::nullopt;
  }
  const auto field = declaration->field;
  const auto semi = declaration->semi;
  auto text_of = [&](const Tok& k) {
    return std::string_view(s).substr(k.begin, k.end - k.begin);
  };
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
                            const std::string& filename,
                            const SchemaSourceReader& read) {
  CatalogResult r;
  auto parser = make_parser();
  if (!parse_schema(*parser, fbs_text, filename, read)) {
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
  // With explicit IDs, FlatBuffers sorts fields by ID. Read the catalog's
  // declaration order from the source before assigning new IDs to the ports.
  const auto declarations =
      catalog_fields(fbs_text, tokens(fbs_text), *catalog);
  auto fields = catalog->fields.vec;
  if (!declarations.empty()) {
    fields.clear();
    for (const auto& decl : declarations) {
      auto* field = catalog->fields.Lookup(decl.name);
      if (!field) {
        r.errors.push_back(filename + ": cannot locate catalog field " +
                           decl.name);
        return r;
      }
      fields.push_back(field);
    }
  }
  for (const auto* field : fields) {
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
                           std::string& out) {
  const auto* s = t.schema();
  int root = -1;
  for (flatbuffers::uoffset_t i = 0; i < s->objects()->size(); ++i)
    if (s->objects()->Get(i) == s->root_table()) root = static_cast<int>(i);
  Renderer r(*s, prefix);
  r.object(root);
  r.emit(out);
  return r.qualified(s->root_table()->name()->str());
}

auto render_input_candidate(const Type& type, const std::string& prefix,
                            InputDeclarations& declarations) -> std::string {
  const auto& schema = *type.schema();
  Renderer renderer(schema, prefix, &declarations);
  const auto* root = schema.root_table();
  const auto name = renderer.qualified(root->name()->str());
  for (flatbuffers::uoffset_t i = 0; i < schema.objects()->size(); ++i)
    if (schema.objects()->Get(i) == root) renderer.object(static_cast<int>(i));
  std::string unused;
  renderer.emit(unused);
  if (const auto it = declarations.types.find(name); it != declarations.types.end()) {
    auto& id = it->second.schema;
    const auto candidate_id = schema_id(type);
    if (!id.empty() && id != candidate_id)
      throw std::runtime_error("conflicting input schema ids for " + name);
    id = candidate_id;
  }
  return name;
}

TypeRef empty_type() {
  static TypeRef t = make_builtin("namespace tsugumi.builtin; table Empty {}",
                                  "tsugumi.builtin.Empty");
  return t;
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

bool json_to_flatbuffer(const Type& t, const std::string& json,
                        std::string* out, std::string* error,
                        bool force_defaults) {
  auto p = parser_from_type(t, error);
  if (!p) return false;
  p->opts.force_defaults = force_defaults;
  p->builder_.ForceDefaults(force_defaults);
  if (!p->ParseJson(json.c_str())) {
    *error = p->error_;
    return false;
  }
  out->assign(reinterpret_cast<const char*>(p->builder_.GetBufferPointer()),
              p->builder_.GetSize());
  return true;
}

}  // namespace tsugumi::types
