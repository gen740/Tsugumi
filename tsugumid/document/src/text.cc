#include "tsugumi/document/text.hh"

#include <flatbuffers/idl.h>

#include <algorithm>
#include <memory>

#include "tsugumi/document/document.hh"

extern const unsigned char tsugumi_project_document_fbs[];
extern const unsigned long tsugumi_project_document_fbs_size;
extern const unsigned char tsugumi_project_lock_fbs[];
extern const unsigned long tsugumi_project_lock_fbs_size;
extern const unsigned char tsugumi_card_document_fbs[];
extern const unsigned long tsugumi_card_document_fbs_size;
extern const unsigned char tsugumi_comments_document_fbs[];
extern const unsigned long tsugumi_comments_document_fbs_size;
extern const unsigned char tsugumi_layout_document_fbs[];
extern const unsigned long tsugumi_layout_document_fbs_size;

namespace tsugumi::document_text {
namespace {

auto source(Schema schema) -> std::string_view {
  auto view = [](const unsigned char* bytes, unsigned long size) {
    return std::string_view(reinterpret_cast<const char*>(bytes), size);
  };
  switch (schema) {
    case Schema::Project:
      return view(tsugumi_project_document_fbs,
                  tsugumi_project_document_fbs_size);
    case Schema::Card:
      return view(tsugumi_card_document_fbs, tsugumi_card_document_fbs_size);
    case Schema::Comments:
      return view(tsugumi_comments_document_fbs,
                  tsugumi_comments_document_fbs_size);
    case Schema::Layout:
      return view(tsugumi_layout_document_fbs,
                  tsugumi_layout_document_fbs_size);
    case Schema::Lock:
      return view(tsugumi_project_lock_fbs, tsugumi_project_lock_fbs_size);
  }
  return {};
}

// One parser per schema and thread: a parser is not shared between threads,
// and it is reused for every document it reads.
auto parser(Schema schema) -> flatbuffers::Parser& {
  thread_local std::unique_ptr<flatbuffers::Parser> parsers[5];
  auto& p = parsers[static_cast<int>(schema)];
  if (!p) {
    flatbuffers::IDLOptions o;
    o.strict_json = true;
    o.natural_utf8 = true;
    o.indent_step = 2;
    o.no_warnings = true;
    p = std::make_unique<flatbuffers::Parser>(o);
    if (!p->Parse(std::string(source(schema)).c_str()))
      throw std::logic_error("document schema: " + p->error_);
  }
  return *p;
}

// "3: 14: error: unknown field: x" -> "line 3, column 14: unknown field: x".
auto describe(const std::string& error) -> std::string {
  const auto mark = error.find("error: ");
  if (mark == std::string::npos) return error;
  auto message = error.substr(mark + 7);
  while (!message.empty() && (message.back() == '\n' || message.back() == ' '))
    message.pop_back();
  const auto where = error.substr(0, mark);
  const auto colon = where.find(':');
  if (colon == std::string::npos) return message;
  std::string line = where.substr(0, colon);
  std::string column = where.substr(colon + 1);
  auto trim = [](std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n'))
      s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == ':')) s.pop_back();
  };
  trim(line);
  trim(column);
  return "line " + line + ", column " + column + ": " + message;
}

// GenText writes an empty vector as "[\n\n  ]" and an empty table as
// "{\n  }". Strings never hold a raw newline (it is escaped), so every
// newline here is structure.
auto collapse_empty(const std::string& text) -> std::string {
  std::string out;
  out.reserve(text.size());
  bool quoted = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted && c == '\\') {
      out += c;
      if (i + 1 < text.size()) out += text[++i];
      continue;
    }
    if (c == '"') quoted = !quoted;
    if (!quoted && (c == '[' || c == '{')) {
      size_t j = i + 1;
      while (j < text.size() && (text[j] == '\n' || text[j] == ' ')) ++j;
      const char close = c == '[' ? ']' : '}';
      if (j > i + 1 && j < text.size() && text[j] == close) {
        out += c;
        out += close;
        i = j;
        continue;
      }
    }
    out += c;
  }
  return out;
}

}  // namespace

auto parse(Schema schema, std::string_view path, std::string_view text)
    -> std::string {
  auto& p = parser(schema);
  std::string owned(text);
  if (owned.find('\0') != std::string::npos)
    throw DocumentError("invalid_document",
                        std::string(path) + ": JSON contains a NUL byte",
                        {{"path", std::string(path)}});
  if (!p.ParseJson(owned.c_str()))
    throw DocumentError("invalid_document",
                        std::string(path) + ": " + describe(p.error_),
                        {{"path", std::string(path)}});
  std::string out(reinterpret_cast<const char*>(p.builder_.GetBufferPointer()),
                  p.builder_.GetSize());
  p.builder_.Clear();
  return out;
}

auto print(Schema schema, std::string_view buffer) -> std::string {
  std::string text;
  if (const char* error = flatbuffers::GenText(parser(schema), buffer.data(),
                                               &text))
    throw DocumentError("invalid_document",
                        std::string("cannot print document: ") + error);
  text = collapse_empty(text);
  if (text.empty() || text.back() != '\n') text += '\n';
  return text;
}

auto canonical(Schema schema, std::string_view path, std::string_view text)
    -> std::string {
  return print(schema, parse(schema, path, text));
}

auto verify(Schema schema, std::string_view buffer) -> bool {
  // Bytes kept in a std::string need not be aligned.
  flatbuffers::Verifier::Options options;
  options.check_alignment = false;
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(buffer.data()),
                          buffer.size(), options);
  switch (schema) {
    case Schema::Project:
      return v.VerifyBuffer<D::Project>(nullptr);
    case Schema::Card:
      return v.VerifyBuffer<D::Card>(nullptr);
    case Schema::Comments:
      return v.VerifyBuffer<D::Comments>(nullptr);
    case Schema::Layout:
      return Tsugumi::Layout::VerifyLayoutBuffer(v);
    case Schema::Lock:
      return v.VerifyBuffer<D::ProjectLock>(nullptr);
  }
  return false;
}

}  // namespace tsugumi::document_text
