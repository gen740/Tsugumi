#include "host/skills.hh"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "agent/helper_scope.hh"
#include "tsugumi/util/fs.hh"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#else
#include <unicode/brkiter.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#endif

extern const unsigned char tsugumi_agent_helpers_skill[];
extern const unsigned long tsugumi_agent_helpers_skill_size;
extern const unsigned char tsugumi_agent_demo_skill[];
extern const unsigned long tsugumi_agent_demo_skill_size;
extern const unsigned char tsugumi_agent_review_skill[];
extern const unsigned long tsugumi_agent_review_skill_size;
extern const unsigned char tsugumi_agent_rerun_compare_skill[];
extern const unsigned long tsugumi_agent_rerun_compare_skill_size;

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace fs = std::filesystem;

#ifdef __APPLE__
struct Release {
  auto operator()(const void* p) const -> void { CFRelease(p); }
};
using String = std::unique_ptr<const __CFString, Release>;
auto Unicode(std::string_view text) -> String {
  return String(CFStringCreateWithBytes(
      nullptr, reinterpret_cast<const UInt8*>(text.data()), text.size(),
      kCFStringEncodingUTF8, false));
}
auto UTF8(CFStringRef s) -> std::string {
  std::string out(CFStringGetMaximumSizeForEncoding(CFStringGetLength(s),
                                                    kCFStringEncodingUTF8) +
                      1,
                  '\0');
  CFStringGetCString(s, out.data(), out.size(), kCFStringEncodingUTF8);
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}
auto Normalize(std::string_view s) -> std::string {
  const auto text = Unicode(s);
  if (!text) return std::string(s);
  auto copy = CFStringCreateMutableCopy(nullptr, 0, text.get());
  CFStringNormalize(copy, kCFStringNormalizationFormC);
  auto result = UTF8(copy);
  CFRelease(copy);
  return result;
}
auto CleanName(std::string_view s) -> std::string {
  const auto text = Unicode(s);
  if (!text) return "";
  std::string out;
  for (CFIndex i = 0; i < CFStringGetLength(text.get());) {
    const auto range =
        CFStringGetRangeOfComposedCharactersAtIndex(text.get(), i);
    const auto first = CFStringGetCharacterAtIndex(text.get(), i);
    UTF32Char scalar = first;
    if (CFStringIsSurrogateHighCharacter(first) && range.length > 1)
      scalar = CFStringGetLongCharacterForSurrogatePair(
          first, CFStringGetCharacterAtIndex(text.get(), i + 1));
    auto classified = CFStringCreateMutable(nullptr, 0);
    if (scalar <= 0xffff) {
      const UniChar c = static_cast<UniChar>(scalar);
      CFStringAppendCharacters(classified, &c, 1);
    } else {
      const UniChar pair[] = {first,
                              CFStringGetCharacterAtIndex(text.get(), i + 1)};
      CFStringAppendCharacters(classified, pair, 2);
    }
    // Swift Character uses the leading scalar's Alphabetic or Number property,
    // not Foundation's broader letter set (which includes all combining marks).
    CFStringTransform(classified, nullptr,
                      CFSTR("[^[:Alphabetic:][:Number:]] Remove"), false);
    if (scalar == '-' || scalar == '_' || CFStringGetLength(classified)) {
      const String part(
          CFStringCreateWithSubstring(nullptr, text.get(), range));
      out += UTF8(part.get());
    } else
      out += '-';
    CFRelease(classified);
    i += range.length;
  }
  return out;
}
#else
auto Normalize(std::string_view s) -> std::string {
  UErrorCode error = U_ZERO_ERROR;
  const auto* normalizer = icu::Normalizer2::getNFCInstance(error);
  icu::UnicodeString normalized;
  normalizer->normalize(icu::UnicodeString::fromUTF8(s), normalized, error);
  std::string out;
  normalized.toUTF8String(out);
  return out;
}
auto CleanName(std::string_view s) -> std::string {
  UErrorCode error = U_ZERO_ERROR;
  std::unique_ptr<icu::BreakIterator> breaks(
      icu::BreakIterator::createCharacterInstance(icu::Locale::getRoot(),
                                                  error));
  const auto text = icu::UnicodeString::fromUTF8(s);
  breaks->setText(text);
  std::string out;
  for (int32_t begin = breaks->first(), end = breaks->next();
       end != icu::BreakIterator::DONE; begin = end, end = breaks->next()) {
    const auto first = text.char32At(begin);
    if (u_hasBinaryProperty(first, UCHAR_ALPHABETIC) ||
        u_getNumericValue(first) != U_NO_NUMERIC_VALUE || first == '-' ||
        first == '_')
      text.tempSubStringBetween(begin, end).toUTF8String(out);
    else
      out += '-';
  }
  return out;
}
#endif

// String(decoding:as:) replaces malformed UTF-8 by maximal invalid subpart.
auto RepairUTF8(std::string_view text) -> std::string {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      out += text[i++];
      continue;
    }
    const size_t length = c >= 0xc2 && c <= 0xdf   ? 2
                          : c >= 0xe0 && c <= 0xef ? 3
                          : c >= 0xf0 && c <= 0xf4 ? 4
                                                   : 0;
    size_t valid = 1;
    while (valid < length && i + valid < text.size()) {
      const auto next = static_cast<unsigned char>(text[i + valid]);
      if (next < 0x80 || next > 0xbf ||
          (valid == 1 &&
           ((c == 0xe0 && next < 0xa0) || (c == 0xed && next > 0x9f) ||
            (c == 0xf0 && next < 0x90) || (c == 0xf4 && next > 0x8f))))
        break;
      ++valid;
    }
    if (length && valid == length)
      out.append(text.substr(i, length));
    else
      out += "\xef\xbf\xbd";
    i += valid;
  }
  return out;
}

auto TrimWhitespace(std::string s) -> std::string {
  // Foundation .whitespaces includes Zs, TAB and ZERO WIDTH SPACE, but no
  // newline characters. Keep this independent of the host's C locale.
  static const std::vector<std::string> spaces = {" ",
                                                  "\t",
                                                  "\xc2\xa0",
                                                  "\xe1\x9a\x80",
                                                  "\xe2\x80\x80",
                                                  "\xe2\x80\x81",
                                                  "\xe2\x80\x82",
                                                  "\xe2\x80\x83",
                                                  "\xe2\x80\x84",
                                                  "\xe2\x80\x85",
                                                  "\xe2\x80\x86",
                                                  "\xe2\x80\x87",
                                                  "\xe2\x80\x88",
                                                  "\xe2\x80\x89",
                                                  "\xe2\x80\x8a",
                                                  "\xe2\x80\x8b",
                                                  "\xe2\x80\xaf",
                                                  "\xe2\x81\x9f",
                                                  "\xe3\x80\x80"};
  size_t begin = 0, end = s.size();
  for (bool found = true; found && begin < end;) {
    found = false;
    for (const auto& space : spaces)
      if (s.compare(begin, space.size(), space) == 0) {
        begin += space.size();
        found = true;
        break;
      }
  }
  for (bool found = true; found && begin < end;) {
    found = false;
    for (const auto& space : spaces)
      if (end - begin >= space.size() &&
          s.compare(end - space.size(), space.size(), space) == 0) {
        end -= space.size();
        found = true;
        break;
      }
  }
  return s.substr(begin, end - begin);
}

auto Trim(std::string s, std::string_view chars) -> std::string {
  const auto begin = s.find_first_not_of(chars);
  return begin == std::string::npos
             ? ""
             : s.substr(begin, s.find_last_not_of(chars) - begin + 1);
}

auto Scan(const fs::path& folder, H::SkillsListT& out) -> void {
  auto source = std::make_unique<H::SkillFolderT>();
  source->path = folder.string();
  source->status = "ready";
  std::error_code error;
  fs::directory_iterator it(folder, error);
  if (error) {
    source->status = "unreadable";
    out.folders.push_back(std::move(source));
    return;
  }
  std::vector<fs::directory_entry> entries;
  for (const fs::directory_iterator end; it != end; it.increment(error)) {
    if (error) break;
    entries.push_back(*it);
  }
  if (error) source->status = "unreadable";
  out.folders.push_back(std::move(source));
  std::stable_sort(entries.begin(), entries.end(),
                   [](const auto& a, const auto& b) {
                     return Normalize(a.path().filename().string()) <
                            Normalize(b.path().filename().string());
                   });
  for (const auto& item : entries) {
    auto file = item.path();
    auto name = file.stem().string();
    // URLResourceValues.isDirectory does not follow directory symlinks.
    if (fs::is_directory(item.symlink_status(error))) {
      name = file.filename().string();
      file /= "SKILL.md";
    } else {
      auto extension = file.extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (extension != ".md") continue;
    }
    auto skill = std::make_unique<H::SkillT>();
    skill->name = Normalize(CleanName(RepairUTF8(name)));
    skill->path = file.string();
    skill->source_folder = folder.string();
    skill->status = "unreadable";
    // Bound the read itself, including a file that grows after stat().
    if (fs::is_regular_file(file, error)) {
      std::ifstream input(file, std::ios::binary);
      if (input) {
        std::string text(kSkillLimit, '\0');
        input.read(text.data(), text.size());
        text.resize(input.gcount());
        if (text.size() == kSkillLimit)
          skill->status = "too_large";
        else if (!input.bad()) {
          skill->status = "ready";
          std::vector<std::string> lines;
          std::istringstream stream(RepairUTF8(text));
          for (std::string line, part; std::getline(stream, part);) {
            line += part;
            if (!part.empty() && part.back() == '\r' && !stream.eof())
              line += '\n';
            else {
              lines.push_back(line);
              line.clear();
            }
          }
          std::string scope_error;
          try {
            if (const auto scope = SkillScope(text)) {
              skill->scoped = true;
              skill->scope = *scope;
            }
          } catch (const std::exception& error) {
            skill->status = "invalid_scope";
            scope_error = error.what();
          }
          size_t body = 0;
          if (!lines.empty() && lines.front() == "---") {
            const auto end = std::find(lines.begin() + 1, lines.end(), "---");
            if (end != lines.end()) {
              body = end - lines.begin() + 1;
              for (auto line = lines.begin() + 1; line != end; ++line) {
                if (line->starts_with("always:"))
                  skill->always = TrimWhitespace(line->substr(7)) == "true";
                if (line->starts_with("name:"))
                  skill->name =
                      Normalize(CleanName(TrimWhitespace(line->substr(5))));
                if (line->starts_with("description:"))
                  skill->description = TrimWhitespace(line->substr(12));
              }
            }
          }
          if (skill->description.empty()) {
            for (size_t i = body; i < lines.size(); ++i)
              if (!TrimWhitespace(lines[i]).empty()) {
                skill->description = Trim(lines[i], "# ");
                break;
              }
          }
          if (!scope_error.empty()) skill->description = scope_error;
        }
      }
    }
    out.skills.push_back(std::move(skill));
  }
}
}  // namespace

auto SkillScope(std::string_view text)
    -> std::optional<std::vector<std::string>> {
  std::istringstream input{std::string(text)};
  std::string line;
  auto next = [&] {
    if (!std::getline(input, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
  };
  if (!next() || line != "---") return std::nullopt;
  std::optional<std::vector<std::string>> result;
  bool always = false;
  while (next()) {
    if (line == "---") {
      if (always && result)
        throw std::runtime_error(
            "A Skill cannot combine always: true with scope; select target "
            "Cards to run a scoped Skill");
      return result;
    }
    if (line.starts_with("always:"))
      always = TrimWhitespace(line.substr(7)) == "true";
    if (!line.starts_with("scope:")) continue;
    if (result) throw std::runtime_error("Duplicate Skill scope declaration");
    auto value = Trim(line.substr(6), " \t");
    if (value.size() < 2 || value.front() != '[' || value.back() != ']')
      throw std::runtime_error("Skill scope must be an inline access list");
    result.emplace();
    std::set<std::string> seen;
    std::istringstream values(value.substr(1, value.size() - 2));
    while (std::getline(values, value, ',')) {
      value = Trim(value, " \t");
      if ((value != "read" && value != "write" && value != "run") ||
          !seen.insert(value).second)
        throw std::runtime_error("Invalid Skill scope access");
      result->push_back(value);
    }
  }
  throw std::runtime_error("Unclosed Skill front matter");
}

auto ScopedSkillTask(std::string_view text, const std::string& name,
                     const std::vector<std::string>& cards,
                     const std::string& instruction)
    -> ::tsugumi::json::Members {
  const auto scope = SkillScope(text);
  if (!scope || cards.empty())
    throw std::runtime_error(
        "A scoped helper requires a declared Skill and selected Cards");
  std::vector<std::string> selected;
  for (const auto& card : cards)
    selected.push_back(::tsugumi::json::Quote(card));
  ::tsugumi::json::Members task{
      {"name", ::tsugumi::json::Quote(name)},
      {"instruction",
       ::tsugumi::json::Quote(instruction + "\n" + std::string(text))}};
  for (const auto* access : {"read", "write", "run"})
    task[std::string(access) + "_cards"] =
        std::find(scope->begin(), scope->end(), access) == scope->end()
            ? "[]"
            : ::tsugumi::json::Encode(selected);
  agent::HelperScope::Validate(task, false);
  return task;
}

auto SelectScopedSkill(const H::SkillsListT& catalog,
                       const std::vector<std::string>& names,
                       const std::string& explicit_name,
                       const std::vector<std::string>& cards,
                       const std::string& instruction)
    -> std::optional<::tsugumi::json::Members> {
  std::set<std::string> selected;
  for (const auto& name : names) selected.insert(SkillNameKey(name));
  std::vector<const H::SkillT*> matches;
  for (const auto& skill : catalog.skills)
    if ((!explicit_name.empty() && skill->name == explicit_name) ||
        (selected.contains(skill->name) && skill->scoped))
      matches.push_back(skill.get());
  if (matches.empty() && explicit_name.empty()) return std::nullopt;
  if (matches.size() != 1 || selected.size() > 1 ||
      matches.front()->status != "ready" || !matches.front()->scoped)
    throw std::runtime_error(
        "Select exactly one unambiguous Skill with a valid scope declaration");
  const auto& skill = *matches.front();
  const auto bytes = skill.path.starts_with("builtin:")
                         ? BundledSkill(skill.path)
                         : ReadSkill(skill.source_folder, skill.path);
  auto task = ScopedSkillTask(bytes, skill.name, cards, instruction);
  task["skill_path"] = ::tsugumi::json::Quote(skill.path);
  return task;
}

auto BundledSkill(const std::string& path) -> std::string {
  if (path == "builtin:helpers")
    return {reinterpret_cast<const char*>(tsugumi_agent_helpers_skill),
            tsugumi_agent_helpers_skill_size};
  if (path == "builtin:demo")
    return {reinterpret_cast<const char*>(tsugumi_agent_demo_skill),
            tsugumi_agent_demo_skill_size};
  if (path == "builtin:review")
    return {reinterpret_cast<const char*>(tsugumi_agent_review_skill),
            tsugumi_agent_review_skill_size};
  if (path == "builtin:rerun-compare")
    return {reinterpret_cast<const char*>(tsugumi_agent_rerun_compare_skill),
            tsugumi_agent_rerun_compare_skill_size};
  return {};
}

auto ReadSkill(const fs::path& root, const fs::path& file) -> std::string {
  // Resolve permitted symlinks, then open every component beneath the resolved
  // root without following links. A replacement after resolution cannot escape.
  const auto base = fs::canonical(root);
  const auto target = fs::canonical(file);
  const auto relative = target.lexically_relative(base);
  if (!tsugumi::fs::valid_relative_path(relative.string()))
    throw std::runtime_error("The selected skill leaves its registered folder");
  std::string bytes;
  tsugumi::fs::stream_beneath(base, relative, kSkillLimit,
                              [&](std::string_view chunk) { bytes += chunk; });
  if (bytes.size() >= kSkillLimit)
    throw std::runtime_error("The selected skill must be smaller than 512 KiB");
  return bytes;
}

auto SkillNameKey(std::string_view name) -> std::string {
  return Normalize(RepairUTF8(name));
}

namespace {
auto DirectoryIdentity(const fs::path& path)
    -> std::optional<std::pair<uint64_t, uint64_t>> {
  struct stat folder{};
  if (::stat(path.c_str(), &folder) != 0 || !S_ISDIR(folder.st_mode))
    return std::nullopt;
  return std::pair{static_cast<uint64_t>(folder.st_dev),
                   static_cast<uint64_t>(folder.st_ino)};
}
auto SaveTrust(sql::Db& db, const fs::path& canonical, bool recursive,
               H::ProjectTrustDecision decision) -> void {
  const auto identity = DirectoryIdentity(canonical);
  if (!identity) throw std::runtime_error("Cannot identify the trusted folder");
  auto save =
      db.prepare("INSERT OR REPLACE INTO skill_trust VALUES(?,?,?,?,?)");
  save.bind(1, canonical.string())
      .bind(2, recursive)
      .bind(3, identity->first)
      .bind(4, identity->second)
      .bind(5, static_cast<int>(decision))
      .run();
}
}  // namespace

auto TrustedFolders(sql::Db& db) -> std::vector<std::string> {
  auto query = db.prepare(
      "SELECT path FROM skill_trust WHERE recursive=1 ORDER BY path");
  std::vector<std::string> folders;
  while (query.step()) folders.push_back(query.text(0));
  return folders;
}

auto SetTrustedFolders(sql::Db& db, const std::vector<std::string>& folders)
    -> void {
  const auto previous = TrustedFolders(db);
  const std::set<std::string> saved(previous.begin(), previous.end());
  std::set<std::string> selected;
  for (const auto& entry : folders) {
    const fs::path path(entry);
    if (!path.is_absolute())
      throw std::runtime_error("Choose an existing absolute folder");
    const auto canonical =
        saved.contains(entry) ? entry : fs::canonical(path).string();
    if (selected.insert(canonical).second && !saved.contains(canonical))
      SaveTrust(db, canonical, true, H::ProjectTrustDecision::Trusted);
  }
  for (const auto& entry : saved) {
    if (selected.contains(entry)) continue;
    auto remove =
        db.prepare("DELETE FROM skill_trust WHERE path=? AND recursive=1");
    remove.bind(1, entry).run();
  }
}

auto ProjectTrust(sql::Db& db, const fs::path& path) -> H::ProjectTrustT {
  H::ProjectTrustT trust;
  if (path.empty()) return trust;
  std::error_code error;
  const auto canonical = fs::canonical(path, error);
  if (error) return trust;
  auto query = db.prepare(
      "SELECT path,recursive,device,inode,decision FROM skill_trust "
      "WHERE recursive=1 OR path=? ORDER BY recursive DESC,path");
  query.bind(1, canonical.string());
  while (query.step()) {
    const fs::path root(query.text(0));
    const bool recursive = query.i64(1) != 0;
    const auto match = std::mismatch(root.begin(), root.end(),
                                     canonical.begin(), canonical.end());
    if (match.first != root.end()) continue;
    if (fs::canonical(root, error) == root && !error &&
        DirectoryIdentity(root) == std::pair{query.u64(2), query.u64(3)}) {
      trust.decision = static_cast<H::ProjectTrustDecision>(query.i64(4));
      if (recursive) trust.trusted_folder = root.string();
      return trust;
    }
    if (!recursive) {
      // Replacing the project directory invalidates its individual decision.
      auto forget =
          db.prepare("DELETE FROM skill_trust WHERE path=? AND recursive=0");
      forget.bind(1, root.string()).run();
    }
  }
  return trust;
}

auto ProjectTrusted(sql::Db& db, const fs::path& path) -> bool {
  return ProjectTrust(db, path).decision == H::ProjectTrustDecision::Trusted;
}

auto SetProjectTrust(sql::Db& db, const fs::path& path,
                     H::ProjectTrustDecision decision) -> void {
  SaveTrust(db, fs::canonical(path), false, decision);
}

auto ListSkills(const fs::path& project,
                const std::vector<std::string>& folders, bool trusted)
    -> H::SkillsListT {
  H::SkillsListT out;
  {
    auto demo = std::make_unique<H::SkillT>();
    demo->name = "demo";
    demo->path = "builtin:demo";
    demo->status = "ready";
    out.skills.push_back(std::move(demo));
    for (const auto* name : {"review", "rerun-compare"}) {
      auto skill = std::make_unique<H::SkillT>();
      skill->name = name;
      skill->path = std::string("builtin:") + name;
      skill->status = "ready";
      skill->scoped = true;
      skill->scope = *SkillScope(BundledSkill(skill->path));
      skill->description = name;
      out.skills.push_back(std::move(skill));
    }
    auto helpers = std::make_unique<H::SkillT>();
    helpers->name = "helpers";
    helpers->path = "builtin:helpers";
    helpers->description = "How to delegate when the person asks";
    helpers->status = "ready";
    helpers->always = true;
    out.skills.push_back(std::move(helpers));
  }
  if (!project.empty()) Scan(project / "skills", out);
  for (const auto& folder : folders)
    Scan(fs::path(folder).lexically_normal(), out);
  // Registered folders are ordered; the last matching source wins. Project
  // sources outrank registered sources, which outrank built-ins.
  std::set<std::string> builtins;
  for (const auto& skill : out.skills)
    if (skill->path.starts_with("builtin:")) builtins.insert(skill->name);
  std::map<std::string, std::string> winners;
  std::map<std::string, int> priorities;
  for (const auto& skill : out.skills) {
    const bool in_project =
        !project.empty() &&
        skill->source_folder == (project / "skills").string();
    const int priority = in_project                            ? 2
                         : skill->path.starts_with("builtin:") ? 0
                                                               : 1;
    if (builtins.contains(skill->name) && (!in_project || trusted) &&
        (!winners.contains(skill->name) ||
         priority >= priorities[skill->name])) {
      winners[skill->name] = skill->path;
      priorities[skill->name] = priority;
    }
  }
  std::erase_if(out.skills, [&](const auto& skill) {
    const bool in_project =
        !project.empty() &&
        skill->source_folder == (project / "skills").string();
    return builtins.contains(skill->name) && (!in_project || trusted) &&
           winners[skill->name] != skill->path;
  });
  for (auto& skill : out.skills) {
    const bool in_project =
        !project.empty() &&
        skill->source_folder == (project / "skills").string();
    if (skill->name == "helpers" && (!in_project || trusted))
      skill->always = true;
    if (skill->always && skill->scoped) {
      skill->status = "invalid_scope";
      skill->description =
          "An automatically applied Skill cannot declare scope; select target "
          "Cards to run a scoped Skill";
    }
    if (in_project && !trusted) skill->always = false;
    if (skill->path.starts_with("builtin:")) continue;
    if (project.empty() ||
        !tsugumi::fs::valid_relative_path(
            fs::path(skill->path).lexically_relative(project).string()))
      skill->attachment_folder = skill->source_folder;
  }
  return out;
}
}  // namespace tsugumi::host
