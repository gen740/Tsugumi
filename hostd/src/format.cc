#include "host/format.hh"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
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
  const auto& rel = request.path;
  if (!fs::valid_relative_path(rel))
    throw ProjectFailure("invalid_path", "invalid path " + rel);
  const auto extension = stdfs::path(rel).extension();
  if (extension != ".json" && extension != ".nix")
    throw ProjectFailure("unsupported", "Formatting supports .json and .nix");
  H::FileFormatResultT result;
  auto text = request.text;
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
  if (extension == ".json") {
    try {
      schema::Json j;
      j.value() = schema::capture(text);
      text = pretty_json(schema::encode(j));
    } catch (const std::exception& e) {
      syntax(e.what(), std::nullopt);
    }
  } else {
    std::string exe = TSUGUMI_FORMAT_NIX;
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
                           "This host build does not provide nixfmt");
    auto dir =
        stdfs::temp_directory_path() / ("tsugumi-format-" + crypto::uuid4());
    const auto file = dir / "source.nix";
    struct Cleanup {
      stdfs::path d;
      ~Cleanup() {
        std::error_code ec;
        stdfs::remove_all(d, ec);
      }
    } cleanup{dir};
    fs::write_atomic(file, text, 0600);
    proc::Spec spec;
    spec.argv = {exe, file.string()};
    // No user/project configs, locale, executable lookup or home directory.
    spec.env = {"LANG=C", "LC_ALL=C", "HOME=" + dir.string(),
                "XDG_CONFIG_HOME=" + dir.string()};
    spec.cwd = dir.string();
    std::string err;
    auto res = proc::run(spec, nullptr, std::nullopt, [&](auto chunk) {
      for (const auto& line : chunk.lines)
        if (err.size() < 8192) err += line;
    });
    if (!proc::exited_ok(res.status)) {
      // Syntax errors: keep the text unchanged and point at the position.
      static const std::regex pos(R"((\d+):(\d+))");
      std::smatch m;
      std::optional<uint64_t> off;
      if (std::regex_search(err, m, pos))
        off = offset_of(text, std::stoull(m[1]), std::stoull(m[2]));
      std::string msg = err.empty() ? "nixfmt failed" : err;
      std::string shown = file.string();
      for (size_t at; (at = msg.find(shown)) != std::string::npos;)
        msg.replace(at, shown.size(), rel);
      syntax(msg, off);
    } else {
      text = fs::read_file_or_throw(file);
    }
  }
  result.text = std::move(text);
  return result;
}

}  // namespace tsugumi::host
