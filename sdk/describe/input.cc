#include "input.hh"

#include <flatbuffers/code_generators.h>
#include <flatbuffers/file_manager.h>
#include <flatbuffers/idl.h>

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "idl_gen_cpp.h"
#include "idl_gen_csharp.h"
#include "idl_gen_go.h"
#include "idl_gen_python.h"
#include "idl_gen_rust.h"

namespace tsugumi::sdk {
namespace {
auto Trim(std::string text) -> std::string {
  const auto begin = text.find_first_not_of(" \t\r\n");
  return begin == std::string::npos
             ? ""
             : text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
auto Replace(std::string& text, const std::string& from, const std::string& to)
    -> void {
  for (size_t at = 0; (at = text.find(from, at)) != std::string::npos;
       at += to.size())
    text.replace(at, from.size(), to);
}
auto Lines(const std::string& text) -> std::vector<std::string> {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  for (std::string line; std::getline(stream, line);) lines.push_back(line);
  return lines;
}
auto Namespace(const flatbuffers::Namespace& ns, const std::string& sep)
    -> std::string {
  std::string text;
  for (const auto& part : ns.components)
    text += (text.empty() ? "" : sep) + part;
  return text;
}
struct MemoryFiles : flatbuffers::FileSaver {
  std::map<std::string, std::string> files;
  auto SaveFile(const char* name, const char* bytes, size_t size, bool)
      -> bool override {
    files[name] = std::string(bytes, size);
    return true;
  }
};
using Files = std::map<std::string, std::string>;
auto Generated(flatbuffers::Parser& parser, const std::string& language)
    -> Files {
  MemoryFiles files;
  parser.opts.generate_object_based_api = true;
  parser.opts.scoped_enums = true;
  parser.opts.prefixed_enums = false;
  parser.opts.cs_global_alias = true;
  parser.opts.file_saver = &files;
  std::unique_ptr<flatbuffers::CodeGenerator> generator;
  if (language == "cpp") generator = flatbuffers::NewCppCodeGenerator();
  if (language == "python") generator = flatbuffers::NewPythonCodeGenerator();
  if (language == "go") generator = flatbuffers::NewGoCodeGenerator();
  if (language == "rust") generator = flatbuffers::NewRustCodeGenerator();
  if (language == "csharp") generator = flatbuffers::NewCSharpCodeGenerator();
  if (generator->GenerateCode(parser, "", "_inputs") !=
      flatbuffers::CodeGenerator::OK)
    throw std::runtime_error("SDK input generation failed: " +
                             generator->status_detail);
  parser.opts.file_saver = nullptr;
  return std::move(files.files);
}
// A declaration's balanced braces, with literals/comments left to the actual
// generator. Generated definitions have no braces in identifiers or comments.
auto Block(const std::string& code, size_t begin) -> std::string {
  const auto brace = code.find('{', begin);
  if (brace == std::string::npos)
    throw std::runtime_error("Missing SDK declaration body");
  int depth = 1;
  size_t end = brace + 1;
  for (; end < code.size() && depth; ++end) {
    if (code[end] == '{') ++depth;
    if (code[end] == '}') --depth;
  }
  if (depth) throw std::runtime_error("Incomplete SDK declaration");
  return code.substr(begin, end - begin);
}
auto Python(flatbuffers::Parser& parser, const Files& files) -> std::string {
  // An annotation-only dataclass describes the generated T object. flatc's
  // annotations call wire strings 'str', but UnPack actually returns bytes.
  std::map<std::string, std::string> names;
  const auto* root = parser.root_struct_def_;
  for (const auto* object : parser.structs_.vec) {
    auto qualified =
        object->defined_namespace->GetFullyQualifiedName(object->name);
    auto name = qualified;
    Replace(name, ".", "__");
    names[qualified + "." + object->name + "T"] =
        object == root ? object->name + "T" : name + "T";
  }
  std::string result =
      "from __future__ import annotations\nfrom dataclasses import "
      "dataclass\nfrom typing import Optional, Union, Any, TYPE_CHECKING\n";
  bool numpy = false;
  auto definition = [&](const flatbuffers::StructDef& object,
                        const std::string& indent) {
    const auto ns = Namespace(*object.defined_namespace, "/");
    const auto path = (ns.empty() ? "" : ns + "/") + object.name + ".py";
    auto found = files.find(path);
    if (found == files.end())
      throw std::runtime_error("Missing Python SDK object " + path);
    const auto& code = found->second;
    const auto start = code.find("class " + object.name + "T(");
    if (start == std::string::npos)
      throw std::runtime_error("Missing Python object API");
    const auto end = code.find("    @classmethod", start);
    std::string body;
    const std::regex field(
        R"(self\.([A-Za-z_][A-Za-z_0-9]*) = .*  # type: (.*))");
    for (const auto& line : Lines(code.substr(start, end - start))) {
      std::smatch match;
      if (!std::regex_search(line, match, field)) continue;
      auto type = match[2].str();
      Replace(type, "List[", "list[");
      type = std::regex_replace(type, std::regex(R"(\bstr\b)"), "bytes");
      // Numeric vectors are lists without NumPy and ndarrays with it.
      if (type.find("list[") != std::string::npos &&
          code.find("self." + match[1].str() + " = ") != std::string::npos &&
          std::regex_search(code, std::regex("self\\." + match[1].str() +
                                             R"( = [^\n]*AsNumpy\()"))) {
        const auto at = type.find("list[");
        const auto close = type.find(']', at);
        type.insert(close + 1, " | numpy.ndarray");
        numpy = true;
      }
      for (const auto& [qualified, short_name] : names)
        Replace(type, qualified,
                short_name == root->name + "T"
                    ? short_name
                    : root->name + "T." + short_name);
      body += indent + "    " + match[1].str() + ": " + type + "\n";
    }
    return body;
  };
  // Dependency classes are nested in the one definition. Forward annotations
  // permit recursive schemas and do not import the generated SDK modules.
  result += "\n@dataclass\nclass " + root->name + "T:\n";
  for (const auto* object : parser.structs_.vec) {
    if (object == root) continue;
    const auto qualified =
        object->defined_namespace->GetFullyQualifiedName(object->name);
    result += "    @dataclass\n    class " +
              names.at(qualified + "." + object->name + "T") + ":\n";
    auto body = definition(*object, "    ");
    result += body.empty() ? "        pass\n" : body;
  }
  auto body = definition(*root, "");
  result += body.empty() ? "    pass\n" : body;
  if (numpy)
    result.insert(result.find('\n', result.find("from typing")) + 1,
                  "if TYPE_CHECKING:\n    import numpy\n");
  return result;
}
auto Cpp(const Files& files) -> std::string {
  std::string result =
      "#include <cstdint>\n#include <memory>\n#include <string>\n#include "
      "<vector>\n#include <array>\n#include <flatbuffers/flatbuffers.h>\n";
  for (const auto& [_, code] : files) {
    std::vector<std::string> namespaces;
    bool native = false, enumeration = false, fixed = false;
    for (const auto& raw : Lines(code)) {
      auto line = Trim(raw);
      if (line.starts_with("FLATBUFFERS_MANUALLY_ALIGNED_STRUCT(")) {
        const auto begin = line.find(") ") + 2;
        result += "struct " +
                  line.substr(begin, line.find(' ', begin) - begin) + " {\n";
        fixed = true;
      } else if (fixed) {
        if (line.starts_with("};")) {
          result += "};\n";
          fixed = false;
        } else if (line == "private:" || line == "public:")
          result += line + "\n";
        else if (line.ends_with(';') && line.find('(') == std::string::npos)
          result += raw + "\n";
        else if (const auto end = line.find("() const {");
                 end != std::string::npos)
          result += "  " + line.substr(0, end + 8) + ";\n";
      } else if (line.starts_with("namespace ") && line.ends_with(" {")) {
        result += line + "\n";
        namespaces.push_back(line);
      } else if (line.starts_with("}  // namespace")) {
        result += "}\n";
        if (!namespaces.empty()) namespaces.pop_back();
      } else if (line.starts_with("enum class ") && line.ends_with(" {")) {
        result += line + "\n";
        enumeration = true;
      } else if (enumeration) {
        result += raw + "\n";
        if (line == "};") enumeration = false;
      } else if (line.starts_with("struct ") && line.ends_with(';') &&
                 !line.ends_with("Builder;") &&
                 line.find(' ') == line.rfind(' ')) {
        result += line + "\n";
      } else if (line.starts_with("struct ") &&
                 (line.find("flatbuffers::NativeTable {") !=
                      std::string::npos ||
                  line.ends_with("Union {"))) {
        result +=
            line.substr(0, line.find(" :")).substr(0, line.find(" {")) + " {\n";
        native = true;
      } else if (native) {
        if (line == "};") {
          result += "};\n";
          native = false;
        } else if (raw.starts_with("  ") && raw.size() > 2 && raw[2] != ' ' &&
                   line.ends_with(';') && !line.starts_with("typedef ") &&
                   !line.starts_with("using ") &&
                   line.find('(') == std::string::npos)
          result += raw + "\n";
      }
    }
  }
  return result;
}
auto Go(const Files& files) -> std::string {
  std::string result = "package input_types\n\n";
  for (const auto& [_, code] : files) {
    for (size_t at = 0; (at = code.find("type ", at)) != std::string::npos;) {
      const auto end = code.find('\n', at);
      const auto line = code.substr(at, end - at);
      if (line.find("T struct {") != std::string::npos)
        result += Block(code, at) + "\n";
      else if (line.find(" struct") == std::string::npos &&
               line.find(" interface") == std::string::npos)
        result += line + "\n";
      at = end;
    }
  }
  return result;
}
auto CSharp(const Files& files) -> std::string {
  std::string result = "using System.Collections.Generic;\n";
  for (const auto& [_, code] : files) {
    const std::regex ns(R"(namespace ([A-Za-z_][A-Za-z_0-9.]*))");
    std::smatch match;
    const bool namespaced = std::regex_search(code, match, ns);
    if (namespaced) {
      auto name = match[1].str();
      Replace(name, ".", ".@");
      result += "namespace @" + name + " {\n";
    }
    for (const auto& raw : Lines(code)) {
      const auto line = Trim(raw);
      if (line.starts_with("public enum ")) {
        const auto begin = code.find(line);
        result += Block(code, begin) + ";\n";
      }
      if (!line.starts_with("public class ")) continue;
      const auto block = Block(code, code.find(line));
      result += line.substr(0, line.find(" {")) + " {\n";
      for (const auto& field : Lines(block))
        if (field.find(" { get; set; }") != std::string::npos) {
          auto declaration = Trim(field);
          declaration = std::regex_replace(
              declaration, std::regex(R"(global::([A-Za-z_][A-Za-z_0-9.]*))"),
              "global::@$1");
          // SDK card_csharp.py escapes every namespace component.
          for (size_t at = 0;
               (at = declaration.find("global::@", at)) != std::string::npos;) {
            const auto end = declaration.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
                "_@.",
                at + 8);
            auto name = declaration.substr(at + 8, end - at - 8);
            Replace(name, ".", ".@");
            declaration.replace(at + 8, end - at - 8, name);
            at += 8 + name.size();
          }
          result += declaration + "\n";
        }
      result += "}\n";
    }
    if (namespaced) result += "}\n";
  }
  return result;
}
auto Rust(const Files& files) -> std::string {
  std::string result;
  // Rust's borrowed view has methods rather than public value fields. Retain
  // just their signatures, one trait per view, with no accessor bodies.
  for (const auto& [_, code] : files) {
    const std::regex view(R"(impl<'a> ([A-Za-z_][A-Za-z_0-9]*)<'a> \{)");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), view);
         it != std::sregex_iterator(); ++it) {
      const auto body = Block(code, static_cast<size_t>(it->position()));
      std::string methods;
      for (const auto& raw : Lines(body)) {
        const auto line = Trim(raw);
        if (line.starts_with("pub fn ") &&
            line.find("(&self)") != std::string::npos &&
            line.find("->") != std::string::npos &&
            !line.starts_with("pub fn unpack("))
          methods += "  " + line.substr(4, line.find(" {") - 4) + ";\n";
      }
      if (!methods.empty())
        result +=
            "pub trait " + (*it)[1].str() + "View<'a> {\n" + methods + "}\n";
    }
  }
  return result;
}
// The dynamic SDKs decode the JSON face (sdk/src/json_face.cc). These are
// ordinary type prototypes, retaining JSON field names instead of flatc names.
class Dynamic {
 public:
  Dynamic(const flatbuffers::Parser& parser, std::string language)
      : parser_(parser), language_(std::move(language)) {}
  auto Render() -> std::string {
    if (language_ == "bash")
      return "cat <<'JSON_SCHEMA'\n" + JsonObject(*parser_.root_struct_def_) +
             "\nJSON_SCHEMA\n";
    auto value = Object(*parser_.root_struct_def_);
    if (language_ == "r") return "Input <- " + value + "\n";
    return "Input = " + value + "\n";
  }

 private:
  const flatbuffers::Parser& parser_;
  std::string language_;
  std::set<const flatbuffers::StructDef*> active_;
  auto Scalar(const flatbuffers::Type& type) -> std::string {
    using namespace flatbuffers;
    if (type.base_type == BASE_TYPE_STRUCT) return Object(*type.struct_def);
    if (type.base_type == BASE_TYPE_VECTOR ||
        type.base_type == BASE_TYPE_ARRAY) {
      auto element = type.VectorType();
      const auto value = Scalar(element);
      if (language_ == "r") {
        if (element.base_type != BASE_TYPE_STRUCT &&
            element.base_type != BASE_TYPE_UNION) {
          auto vector = value;
          Replace(vector, "(1)", "()");
          return vector;
        }
        return "list(" + value + ")";
      }
      return "[" + value + "]";
    }
    if (type.base_type == BASE_TYPE_UNION) {
      std::string members;
      for (const auto* member : type.enum_def->Vals()) {
        if (!member->union_type.struct_def) continue;
        members += (members.empty() ? "" : ", ") +
                   Object(*member->union_type.struct_def);
      }
      if (language_ == "r") return "list(" + members + ")";
      return "[" + members + "]";
    }
    if (type.base_type == BASE_TYPE_STRING || type.enum_def) {
      if (language_ == "r") return "character(1)";
      return "String";
    }
    if (type.base_type == BASE_TYPE_BOOL) {
      if (language_ == "r") return "logical(1)";
      if (language_ == "julia") return "Bool";
      return "[TrueClass, FalseClass]";
    }
    if (IsFloat(type.base_type)) {
      if (language_ == "r") return "numeric(1)";
      return language_ == "julia" ? "Float64" : "Float";
    }
    if (language_ == "r") return "numeric(1)";
    if (language_ == "julia")
      return type.base_type == BASE_TYPE_ULONG ? "Union{Int64,UInt64}"
                                               : "Int64";
    return "Integer";
  }
  auto Object(const flatbuffers::StructDef& object) -> std::string {
    if (!active_.insert(&object).second)
      return language_ == "r"       ? "list()"
             : language_ == "julia" ? "Dict{String,Any}"
                                    : "Hash";
    std::string fields;
    for (const auto* field : object.fields.vec) {
      if (field->deprecated) continue;
      fields += fields.empty() ? "" : ", ";
      auto value = Scalar(field->value.type);
      if (language_ == "r")
        fields += "`" + field->name + "` = " + value;
      else
        fields += "\"" + field->name + "\" => " + value;
    }
    active_.erase(&object);
    if (language_ == "r") return "list(" + fields + ")";
    if (language_ == "julia") return "Dict{String,Any}(" + fields + ")";
    return "{" + fields + "}";
  }
  auto JsonType(const flatbuffers::Type& type) -> std::string {
    using namespace flatbuffers;
    if (type.base_type == BASE_TYPE_STRUCT) return JsonObject(*type.struct_def);
    if (type.base_type == BASE_TYPE_VECTOR || type.base_type == BASE_TYPE_ARRAY)
      return "{\"type\":\"array\",\"items\":" + JsonType(type.VectorType()) +
             "}";
    if (type.base_type == BASE_TYPE_UNION) {
      std::string members;
      for (const auto* member : type.enum_def->Vals()) {
        if (!member->union_type.struct_def) continue;
        members += (members.empty() ? "" : ",") +
                   JsonObject(*member->union_type.struct_def);
      }
      return "{\"oneOf\":[" + members + "]}";
    }
    const auto kind = type.base_type == BASE_TYPE_STRING || type.enum_def
                          ? "string"
                      : type.base_type == BASE_TYPE_BOOL ? "boolean"
                      : IsFloat(type.base_type)          ? "number"
                                                         : "integer";
    return "{\"type\":\"" + std::string(kind) + "\"}";
  }
  auto JsonObject(const flatbuffers::StructDef& object) -> std::string {
    if (!active_.insert(&object).second) return "{\"$ref\":\"#\"}";
    std::string fields;
    for (const auto* field : object.fields.vec) {
      if (field->deprecated) continue;
      fields += (fields.empty() ? "" : ",") + std::string("\"") + field->name +
                "\":" + JsonType(field->value.type);
    }
    active_.erase(&object);
    return "{\"type\":\"object\",\"properties\":{" + fields + "}}";
  }
};
}  // namespace

auto DescribeInput(const std::string& schema, const std::string& language)
    -> std::string {
  flatbuffers::Parser parser;
  if (!parser.Parse(schema.c_str()) || !parser.root_struct_def_)
    throw std::runtime_error("Invalid input schema: " + parser.error_);
  if (language == "r" || language == "julia" || language == "ruby" ||
      language == "bash")
    return Dynamic(parser, language).Render();
  if (language != "cpp" && language != "python" && language != "go" &&
      language != "rust" && language != "csharp")
    return schema;
  const auto generated = Generated(parser, language);
  if (language == "python") return Python(parser, generated);
  if (language == "cpp") return Cpp(generated);
  if (language == "go") return Go(generated);
  if (language == "rust") return Rust(generated);
  return CSharp(generated);
}
}  // namespace tsugumi::sdk
