#include "host/format.hh"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <regex>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "host/projects.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json.hh"
#include "tsugumi/util/process.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace D = Tsugumi::Data;
namespace stdfs = std::filesystem;

namespace {

// Indents compact JSON (strings are copied verbatim, so escapes survive).
auto pretty_json(const std::string& raw) -> std::string {
  std::string compact;
  bool quoted = false, escaped = false;
  for (unsigned char c : raw) {
    if (quoted || !std::isspace(c)) compact += static_cast<char>(c);
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
  }
  std::string out;
  int depth = 0;
  bool in_str = false;
  bool esc = false;
  auto newline = [&] {
    out += '\n' + std::string(static_cast<size_t>(depth) * 2, ' ');
  };
  for (size_t i = 0; i < compact.size(); ++i) {
    char c = compact[i];
    if (in_str) {
      out += c;
      if (esc)
        esc = false;
      else if (c == '\\')
        esc = true;
      else if (c == '"')
        in_str = false;
      continue;
    }
    switch (c) {
      case '"':
        in_str = true;
        out += c;
        break;
      case '{':
      case '[':
        out += c;
        if (i + 1 < compact.size() &&
            (compact[i + 1] == '}' || compact[i + 1] == ']')) {
          out += compact[++i];
          break;
        }
        ++depth;
        newline();
        break;
      case '}':
      case ']':
        --depth;
        newline();
        out += c;
        break;
      case ',':
        out += c;
        newline();
        break;
      case ':':
        out += ": ";
        break;
      default:
        out += c;
    }
  }
  return out + "\n";
}

auto language_of(const std::string& path) -> std::optional<D::FormatLanguage> {
  auto ext = stdfs::path(path).extension().string();
  if (ext == ".json") return D::FormatLanguage::json;
  if (ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".hh" ||
      ext == ".hpp" || ext == ".h")
    return D::FormatLanguage::cpp;
  if (ext == ".py") return D::FormatLanguage::python;
  if (ext == ".nix") return D::FormatLanguage::nix;
  if (ext == ".sh" || ext == ".bash") return D::FormatLanguage::bash;
  if (ext == ".fbs") return D::FormatLanguage::fbs;
  return std::nullopt;
}

// Byte offset of 1-based line/column in text.
auto offset_of(const std::string& text, uint64_t line, uint64_t col)
    -> uint64_t {
  uint64_t off = 0;
  uint64_t l = 1;
  while (l < line && off < text.size()) {
    if (text[off] == '\n') ++l;
    ++off;
  }
  return std::min<uint64_t>(off + (col > 0 ? col - 1 : 0), text.size());
}

}  // namespace

auto Format(const H::FileFormatRequestT& request) -> H::FileFormatResultT {
  const auto* r = &request;
  std::string rel = r->path;
  if (!fs::valid_relative_path(rel))
    throw ProjectFailure("invalid_path", "invalid path " + rel);
  auto lang = r->language != D::FormatLanguage::unspecified
                  ? std::optional(r->language)
                  : language_of(rel);
  if (!lang)
    throw ProjectFailure("unsupported", "the language of " + rel +
                                            " is not known; pass language");
  H::FileFormatResultT result;
  std::string text = r->text;
  auto syntax = [&](const std::string& message,
                    std::optional<uint64_t> offset) {
    auto d = std::make_unique<D::DiagnosticT>();
    d->severity = D::Severity::error;
    d->code = "syntax_error";
    d->message = message;
    d->path = rel;
    d->offset = offset ? flatbuffers::Optional<uint64_t>(*offset)
                       : flatbuffers::nullopt;
    result.diagnostics.push_back(std::move(d));
  };
  if (*lang == D::FormatLanguage::json) {
    try {
      schema::Json j;
      j.value() = schema::capture(text);
      text = pretty_json(schema::encode(j));
    } catch (const std::exception& e) {
      syntax(e.what(), std::nullopt);
    }
  } else {
    // tool, arguments before the file, whether it rewrites the file in place
    struct Tool {
      const char* exe;
      std::vector<std::string> args;
      bool in_place;
      const char* ext;
    };
    Tool tool;
    switch (*lang) {
      case D::FormatLanguage::cpp:
        tool = {TSUGUMI_FORMAT_CLANG,
                {"--style={BasedOnStyle: Google, ColumnLimit: 110}",
                 "--fallback-style=none"},
                false,
                ".cc"};
        break;
      case D::FormatLanguage::python:
        tool = {TSUGUMI_FORMAT_RUFF,
                {"format", "--isolated", "--no-cache", "--target-version",
                 "py313", "--line-length", "88"},
                true,
                ".py"};
        break;
      case D::FormatLanguage::nix:
        tool = {TSUGUMI_FORMAT_NIX, {}, true, ".nix"};
        break;
      case D::FormatLanguage::bash:
        tool = {TSUGUMI_FORMAT_SH, {"-i", "2", "-ln", "bash"}, false, ".sh"};
        break;
      default:
        throw ProjectFailure("unsupported",
                             "no formatter is provided for this language");
    }
    std::string exe = tool.exe;
#ifdef __APPLE__
    if (exe.starts_with("@bundle/")) {
      uint32_t size = 0;
      _NSGetExecutablePath(nullptr, &size);
      std::string path(size, '\0');
      if (_NSGetExecutablePath(path.data(), &size) != 0)
        throw ProjectFailure("unavailable", "Cannot locate host executable");
      exe = (stdfs::canonical(stdfs::path(path.c_str())).parent_path() /
             "../Resources/formatters" / exe.substr(8))
                .lexically_normal()
                .string();
    }
#endif
    if (exe.empty() || !stdfs::path(exe).is_absolute())
      throw ProjectFailure("unsupported",
                           "This host build does not provide the formatter");
    auto dir =
        stdfs::temp_directory_path() / ("tsugumi-format-" + crypto::uuid4());
    auto file = dir / (std::string("source") + tool.ext);
    fs::write_atomic(file, text, 0600);
    struct Cleanup {
      stdfs::path d;
      ~Cleanup() {
        std::error_code ec;
        stdfs::remove_all(d, ec);
      }
    } cleanup{dir};
    proc::Spec spec;
    spec.argv = {exe};
    spec.argv.insert(spec.argv.end(), tool.args.begin(), tool.args.end());
    spec.argv.push_back(file.string());
    // No user/project configs, locale, executable lookup or home directory.
    spec.env = {"LANG=C", "LC_ALL=C", "HOME=" + dir.string(),
                "XDG_CONFIG_HOME=" + dir.string(),
                "SHFMT_NO_EDITORCONFIG=true"};
    spec.cwd = dir.string();
    std::string err;
    auto res =
        proc::run(spec, nullptr, std::nullopt, [&](const std::string& line) {
          if (err.size() < 8192) err += line;
        });
    if (!proc::exited_ok(res.status)) {
      // Syntax errors: keep the text unchanged and point at the position.
      static const std::regex pos(R"((\d+):(\d+))");
      std::smatch m;
      std::optional<uint64_t> off;
      if (std::regex_search(err, m, pos))
        off = offset_of(text, std::stoull(m[1]), std::stoull(m[2]));
      std::string msg = err.empty() ? std::string(tool.exe) + " failed" : err;
      std::string shown = file.string();
      for (size_t at; (at = msg.find(shown)) != std::string::npos;)
        msg.replace(at, shown.size(), rel);
      syntax(msg, off);
    } else {
      text = tool.in_place ? fs::read_file_or_throw(file) : res.out;
    }
  }
  result.text = std::move(text);
  return result;
}

}  // namespace tsugumi::host
