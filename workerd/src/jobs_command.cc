// `tsugumi-workerd jobs`: what job data is left on this worker, and its
// removal by the person on this machine. It shows; the person decides.
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <span>
#include <utility>

#include "journal.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json.hh"

namespace tsugumi::workerd {
namespace {
struct Fd {
  int value = -1;
  explicit Fd(int fd) : value(fd) {}
  Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
  auto operator=(Fd&&) -> Fd& = delete;
  ~Fd() {
    if (value >= 0) ::close(value);
  }
};

struct JobJson {
  schema::t::String<"project_scope"> project_scope;
  schema::t::String<"job_id"> job_id;
  schema::t::String<"state"> state;
  schema::t::Uint64<"bytes"> bytes;
  schema::t::String<"last_written"> last_written;
  schema::t::String<"core_life_id"> core_life_id;
  schema::t::String<"core_last_seen"> core_last_seen;
  schema::t::String<"reason"> reason;
};
struct ListJson {
  schema::t::String<"state_dir"> state_dir;
  schema::t::Array<"jobs", schema::Obj<JobJson>> jobs;
};

struct Entry {
  std::shared_ptr<Journal> journal;
  std::string scope;
  std::string job;
};

constexpr std::string_view kUsage =
    "usage: tsugumi-workerd jobs list [--state-dir DIR] [--all] [--json]\n"
    "       tsugumi-workerd jobs remove [--state-dir DIR] [--active] "
    "PROJECT/JOB...\n";

auto entries(const stdfs::path& root) -> std::vector<Entry> {
  std::vector<Entry> result;
  const auto projects = root / "projects";
  if (!stdfs::is_directory(projects)) return result;
  std::vector<std::string> scopes;
  for (const auto& entry : stdfs::directory_iterator(projects))
    if (crypto::is_sha256_hex(entry.path().filename().string()))
      scopes.push_back(entry.path().filename().string());
  std::ranges::sort(scopes);
  for (const auto& scope : scopes) {
    auto journal = std::make_shared<Journal>(projects / scope);
    auto jobs = journal->jobs();
    std::ranges::sort(jobs);
    for (auto& job : jobs) result.push_back({journal, scope, std::move(job)});
  }
  return result;
}

// Takes the job's supervisor lock without waiting. The descriptor is invalid
// when another process holds it: a supervisor is running the job.
auto lock_supervisor(const Journal& journal, const std::string& job) -> Fd {
  Fd lock(::open(journal.supervisor_lock_path(job).c_str(),
                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (lock.value < 0)
    throw std::runtime_error("cannot open the job's supervisor lock");
  if (::flock(lock.value, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock.value);
    lock.value = -1;
  }
  return lock;
}

struct Usage {
  uint64_t bytes = 0;
  std::time_t written = 0;
  bool scratch = false;  // anything besides the state record
};
auto usage(const stdfs::path& directory) -> Usage {
  Usage result;
  const auto add = [&](const stdfs::path& path) {
    struct stat s{};
    if (::lstat(path.c_str(), &s) != 0) return;
    result.bytes += static_cast<uint64_t>(s.st_blocks) * 512;
    result.written = std::max(result.written, s.st_mtim.tv_sec);
  };
  add(directory);
  std::error_code ec;
  for (stdfs::recursive_directory_iterator
           i(directory, stdfs::directory_options::skip_permission_denied, ec),
       end;
       !ec && i != end; i.increment(ec)) {
    add(i->path());
    if (i.depth() == 0 && i->path().filename() != "state")
      result.scratch = true;
  }
  return result;
}

auto lower(std::string_view text) -> std::string {
  std::string result(text);
  for (auto& c : result)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}
auto timestamp(std::time_t time) -> std::string {
  std::tm tm{};
  ::gmtime_r(&time, &tm);
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return text;
}
auto human(uint64_t bytes) -> std::string {
  constexpr const char* units[]{"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024 && unit + 1 < std::size(units)) {
    value /= 1024;
    ++unit;
  }
  char text[32];
  std::snprintf(text, sizeof text, unit ? "%.1f %s" : "%.0f %s", value,
                units[unit]);
  return text;
}

auto reason(const Record& record, bool supervised, bool scratch)
    -> std::string {
  switch (record.state) {
    case R::JobState::Missing:
      return "no journal record: staging stopped before the first write";
    case R::JobState::Uploading:
      return "inputs staged, never submitted";
    case R::JobState::Queued:
      return "queued, not started";
    case R::JobState::Running:
    case R::JobState::Cancelling:
      if (supervised)
        return lower(R::EnumNameJobState(record.state)) + " (supervisor pid " +
               std::to_string(record.pid) + ")";
      return "recorded as " + lower(R::EnumNameJobState(record.state)) +
             " but no supervisor runs it; a running listener records it as "
             "interrupted";
    case R::JobState::Succeeded:
      return "not released: the result was never collected";
    case R::JobState::Failed:
    case R::JobState::Cancelled:
    case R::JobState::Interrupted:
      return "not released: inputs, logs and partial results are kept";
    case R::JobState::Released:
      return scratch ? "released, erasing did not finish; a running listener "
                       "finishes it"
                     : "released: only the tombstone that rejects a late "
                       "submission is left";
  }
  return {};
}

auto list_jobs(const stdfs::path& root, bool all, bool json) -> int {
  ListJson output;
  *output.state_dir = root.string();
  for (const auto& [journal, scope, job] : entries(root)) {
    JobJson row;
    *row.project_scope = scope;
    *row.job_id = job;
    *row.core_last_seen = "unknown";
    const bool supervised = lock_supervisor(*journal, job).value < 0;
    const auto disk = usage(journal->job_path(job));
    try {
      const auto record = journal->status(job);
      if (record.state == R::JobState::Released && !disk.scratch && !all)
        continue;
      *row.state = lower(R::EnumNameJobState(record.state));
      *row.core_life_id = record.core_life_id;
      *row.reason = reason(record, supervised, disk.scratch);
    } catch (const std::exception& e) {
      *row.state = "unreadable";
      *row.reason = std::string("journal record is unreadable: ") + e.what();
    }
    *row.bytes = disk.bytes;
    *row.last_written = timestamp(disk.written);
    output.jobs->push_back(std::move(row));
  }
  if (json) {
    std::cout << schema::encode(output) << '\n';
    return 0;
  }
  if (output.jobs->empty()) {
    std::cout << "No job data in " << root.string() << '\n';
    return 0;
  }
  // Prefixes are enough to name a job to `jobs remove`; --json has full IDs.
  std::printf("%-13s %-13s %-12s %10s  %-20s  %-13s %-10s %s\n", "PROJECT",
              "JOB", "STATE", "SIZE", "LAST WRITTEN", "CORE", "CORE SEEN",
              "WHY");
  for (const auto& row : *output.jobs) {
    const auto core =
        row.core_life_id->empty() ? "-" : row.core_life_id->substr(0, 12);
    std::printf("%-13s %-13s %-12s %10s  %-20s  %-13s %-10s %s\n",
                row.project_scope->substr(0, 12).c_str(),
                row.job_id->substr(0, 12).c_str(), row.state->c_str(),
                human(*row.bytes).c_str(), row.last_written->c_str(),
                core.c_str(), row.core_last_seen->c_str(), row.reason->c_str());
  }
  std::cout << "The worker does not record when a core was last seen.\n";
  return 0;
}

auto hex(std::string_view text) -> bool {
  return std::ranges::all_of(text, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
// PROJECT/JOB, each an unambiguous prefix of at least 8 hex characters.
auto find_job(const std::vector<Entry>& all, std::string_view name)
    -> const Entry& {
  const auto slash = name.find('/');
  const auto scope = slash == std::string_view::npos ? std::string_view{}
                                                     : name.substr(0, slash);
  const auto job = slash == std::string_view::npos ? std::string_view{}
                                                   : name.substr(slash + 1);
  if (scope.size() < 8 || job.size() < 8 || !hex(scope) || !hex(job))
    throw std::runtime_error(
        std::string(name) +
        ": name a job as PROJECT/JOB with at least 8 hex characters of each");
  const Entry* found = nullptr;
  for (const auto& entry : all)
    if (entry.scope.starts_with(scope) && entry.job.starts_with(job)) {
      if (found)
        throw std::runtime_error(std::string(name) +
                                 ": matches more than one job");
      found = &entry;
    }
  if (!found) throw std::runtime_error(std::string(name) + ": no such job");
  return *found;
}

auto remove_jobs(const stdfs::path& root, bool active,
                 const std::vector<std::string>& names) -> int {
  const auto all = entries(root);
  std::vector<const Entry*> chosen;
  for (const auto& name : names) chosen.push_back(&find_job(all, name));
  int status = 0;
  for (const auto* entry : chosen) {
    const auto name = entry->scope + "/" + entry->job;
    try {
      // Holding the supervisor lock keeps the listener from starting the job.
      const auto lock = lock_supervisor(*entry->journal, entry->job);
      if (lock.value < 0)
        throw std::runtime_error(
            "a supervisor process is running it; cancel the run first");
      const auto disk = usage(entry->journal->job_path(entry->job));
      Record previous;
      try {
        previous = entry->journal->discard(entry->job, active);
      } catch (const Error& e) {
        if (e.code != R::ErrorCode::Rejected) throw;
        throw std::runtime_error(
            std::string(e.what()) +
            "; when nothing should run it any more, add --active");
      }
      if (previous.state == R::JobState::Released && !disk.scratch)
        std::cout << "already released: " << name << '\n';
      else
        std::cout << "removed " << name << " ("
                  << lower(R::EnumNameJobState(previous.state)) << ", "
                  << human(disk.bytes) << ")\n";
    } catch (const std::exception& e) {
      std::cerr << "tsugumi-workerd: not removed " << name << ": " << e.what()
                << '\n';
      status = 1;
    }
  }
  return status;
}
}  // namespace

auto jobs_main(stdfs::path root, std::span<char*> args) -> int {
  if (args.empty()) {
    std::cerr << kUsage;
    return 2;
  }
  const std::string_view command(args[0]);
  bool all = false, json = false, active = false;
  std::vector<std::string> names;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string_view option(args[i]);
    if (option == "--state-dir" && i + 1 < args.size())
      root = args[++i];
    else if (option == "--all" && command == "list")
      all = true;
    else if (option == "--json" && command == "list")
      json = true;
    else if (option == "--active" && command == "remove")
      active = true;
    else if (command == "remove" && !option.starts_with("--"))
      names.emplace_back(option);
    else {
      std::cerr << kUsage;
      return 2;
    }
  }
  if (!stdfs::is_directory(root))
    throw std::runtime_error("no remote worker state in " + root.string());
  if (command == "list") return list_jobs(root, all, json);
  if (command == "remove" && !names.empty())
    return remove_jobs(root, active, names);
  std::cerr << kUsage;
  return 2;
}
}  // namespace tsugumi::workerd
