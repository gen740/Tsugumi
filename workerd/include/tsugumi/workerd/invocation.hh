#pragma once

#include <filesystem>

#include "schema.hh"

namespace tsugumi {
namespace remote {

struct FileJson {
  schema::t::String<"path"> path;
  schema::t::String<"sha256"> sha256;
  schema::t::Uint64<"size"> size;
};

struct InvocationJson {
  schema::t::String<"project_id"> project_id;
  schema::t::String<"call_sha256"> call_sha256;
  schema::t::String<"archive_sha256"> archive_sha256;
  schema::t::Array<"files", schema::Obj<FileJson>> files;
  schema::t::Array<"directories", schema::v::String> directories;
  schema::t::Bool<"network"> network;
  schema::t::Bool<"filesystem"> filesystem;
  schema::t::Uint64<"implementation_index"> implementation_index;
};

struct EnvironmentJson {
  schema::t::Array<"paths", schema::v::String> paths;
  schema::t::Array<"directories", schema::v::String> directories;
  schema::t::Bool<"built"> built;
};

struct LogJson {
  schema::t::Uint64<"stream"> stream;
  schema::t::String<"text"> text;
};

auto relative_path(std::string_view path) -> std::filesystem::path;
auto store_path(std::string_view path) -> bool;

}  // namespace remote
}  // namespace tsugumi
