#include "dashboard.hh"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <set>
#include <stdexcept>
#include <utility>

#include "dashboard_page.hh"
#include "jobs_command.hh"
#include "tsugumi/rpc/password.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json.hh"
#include "tsugumi/workerd/protocol.hh"

namespace tsugumi::workerd {
namespace {
namespace HTTP = dashboard_http;
using namespace std::chrono_literals;
namespace t = schema::t;

constexpr std::string_view kCookie = "__Host-tsugumi";
constexpr auto kSessionLife = 12h;
constexpr size_t kSessions = 64;
// Waits after the fifth, sixth, seventh and later failed checks in a row.
constexpr std::chrono::minutes kLockouts[] = {1min, 3min, 5min, 30min};
constexpr unsigned kFreeAttempts = 4;

struct ErrorJson {
  t::String<"error"> error;
};
struct LoginJson {
  t::Bool<"configured"> configured;
  t::Uint64<"locked_seconds"> locked_seconds;
};
struct PasswordErrorJson {
  t::String<"error"> error;
  t::Uint64<"locked_seconds"> locked_seconds;
};
struct WorkerJson {
  t::String<"name"> name;
  t::String<"version"> version;
  t::Uint32<"contract"> contract;
  t::String<"revision"> revision;
  t::String<"system"> system;
  t::String<"server_id"> server_id;
  t::Uint64<"uptime_seconds"> uptime_seconds;
  t::String<"listen"> listen;
  t::Uint32<"dashboard_port"> dashboard_port;
  t::Uint32<"running"> running;
  t::Bool<"restart_pending"> restart_pending;
  t::Bool<"restart_required"> restart_required;
};
struct JobJson {
  t::String<"project_scope"> project_scope;
  t::String<"job_id"> job_id;
  t::String<"state"> state;
  t::Uint64<"bytes"> bytes;
  t::String<"last_written"> last_written;
  t::String<"reason"> reason;
};
struct DiskJson {
  // False until the first walk of the store and the jobs has finished.
  t::Bool<"measured"> measured;
  t::Uint64<"store_bytes"> store_bytes;
  t::Uint64<"jobs_bytes"> jobs_bytes;
  t::Uint64<"free_bytes"> free_bytes;
  t::Uint64<"total_bytes"> total_bytes;
};
struct PullJson {
  t::String<"url"> url;
  t::String<"public_key"> public_key;
};
struct SettingsJson {
  t::String<"name"> name;
  t::String<"listen_address"> listen_address;
  t::Uint16<"port"> port;
  t::Uint16<"dashboard_port"> dashboard_port;
  t::Uint32<"max_jobs"> max_jobs;
  t::Uint32<"nix_max_jobs"> nix_max_jobs;
  t::Uint32<"nix_cores"> nix_cores;
  t::Array<"pull_caches", schema::Obj<PullJson>> pull_caches;
};
struct HostJson {
  t::String<"key"> key;
  t::String<"comment"> comment;
  t::String<"last_seen"> last_seen;
};
struct StateJson {
  t::String<"role"> role;
  t::String<"csrf"> csrf;
  t::Field<"login", schema::Obj<LoginJson>> login;
  t::Field<"worker", schema::Obj<WorkerJson>> worker;
  t::Array<"jobs", schema::Obj<JobJson>> jobs;
  t::Field<"disk", schema::Obj<DiskJson>> disk;
  t::Field<"settings", schema::Obj<SettingsJson>> settings;
  t::Array<"hosts", schema::Obj<HostJson>> hosts;
};
struct RemovalJson {
  t::String<"job"> job;
  t::String<"status"> status;
  t::String<"detail"> detail;
};
struct RemovalsJson {
  t::Array<"results", schema::Obj<RemovalJson>> results;
};

auto Json(std::string body, unsigned status = 200) -> HTTP::Reply {
  return {status, std::string(HTTP::kJson), std::move(body), {}};
}
auto ErrorReply(unsigned status, std::string_view error) -> HTTP::Reply {
  ErrorJson result;
  *result.error = error;
  return Json(schema::encode(result), status);
}
auto Status(JobRemovalStatus status) -> std::string_view {
  switch (status) {
    case JobRemovalStatus::Removed:
      return "removed";
    case JobRemovalStatus::AlreadyReleased:
      return "released";
    case JobRemovalStatus::ActiveConfirmationRequired:
      return "active";
    case JobRemovalStatus::Supervised:
      return "supervised";
    case JobRemovalStatus::Failed:
      return "failed";
  }
  return "failed";
}
auto JobName(std::string_view value) -> bool {
  return value.size() == 129 && value[64] == '/' &&
         crypto::is_sha256_hex(value.substr(0, 64)) &&
         crypto::is_sha256_hex(value.substr(65));
}
auto Page(bool japanese) -> HTTP::Reply {
  auto html = std::string(kDashboardPage);
  if (japanese) html.replace(html.find("lang=\"en\""), 9, "lang=\"ja\"");
  return {200, std::string(HTTP::kHtml), std::move(html), {}};
}
auto Utc(Dashboard::Wall::time_point time) -> std::string {
  const auto seconds = Dashboard::Wall::to_time_t(time);
  std::tm utc{};
  ::gmtime_r(&seconds, &utc);
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}
// Allocated bytes under `directory`, counting each hard-linked file once.
auto DiskBytes(const std::filesystem::path& directory,
               const std::atomic<bool>& stop) -> uint64_t {
  uint64_t bytes = 0;
  std::set<std::pair<dev_t, ino_t>> seen;
  const auto add = [&](const std::filesystem::path& path) {
    struct stat s{};
    if (::lstat(path.c_str(), &s) == 0 &&
        (s.st_nlink < 2 || seen.emplace(s.st_dev, s.st_ino).second))
      bytes += static_cast<uint64_t>(s.st_blocks) * 512;
  };
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) return 0;
  add(directory);
  for (std::filesystem::recursive_directory_iterator
           i(directory,
             std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       !ec && i != end && !stop.load(std::memory_order_relaxed);
       i.increment(ec))
    add(i->path());
  return bytes;
}
auto HostName() -> std::string {
  char name[256] = {};
  if (::gethostname(name, sizeof name - 1) != 0) return "worker";
  return name;
}
// The comment after a key on its authorized_keys line, without a leading #.
auto Comment(std::string_view line, std::string_view key) -> std::string {
  auto rest = line.substr(std::min(line.size(), line.find(key) + key.size()));
  const auto trim = [](std::string_view& text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '#'))
      text.remove_prefix(1);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' ' || text.back() == '\t'))
      text.remove_suffix(1);
  };
  trim(rest);
  return std::string(rest);
}
auto Header(const HTTP::Request& request, const std::string& name)
    -> std::string_view {
  const auto found = request.headers.find(name);
  return found == request.headers.end() ? std::string_view{}
                                        : std::string_view(found->second);
}
}  // namespace

Dashboard::Dashboard(std::filesystem::path root, WorkerSettings& settings,
                     Listener listener)
    : root_(std::move(root)),
      keys_(root_ / "authorized_keys"),
      settings_(settings),
      listener_(std::move(listener)) {}

Dashboard::~Dashboard() {
  stopping_ = true;
  if (walker_.joinable()) walker_.join();
}

auto Dashboard::serve(peer_tls::Stream& stream) -> void {
  HTTP::Reply reply;
  try {
    const auto request = HTTP::ReadRequest(stream, {});
    try {
      reply = handle(request);
      if (reply.body.size() > HTTP::kReplyLimit)
        reply = ErrorReply(500, "Dashboard reply is too large");
    } catch (const std::invalid_argument& e) {
      reply = ErrorReply(400, e.what());
    } catch (const std::exception& e) {
      reply = ErrorReply(409, e.what());
    }
  } catch (const std::exception&) {
    reply = ErrorReply(400, "Invalid dashboard request");
  }
  HTTP::WriteReply(stream, reply);
}

auto Dashboard::session(const HTTP::Request& request) -> const Session* {
  auto cookies = Header(request, "cookie");
  std::string token;
  while (!cookies.empty()) {
    auto item = cookies.substr(0, cookies.find(';'));
    cookies.remove_prefix(std::min(cookies.size(), item.size() + 1));
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    if (item.starts_with(kCookie) && item.size() > kCookie.size() &&
        item[kCookie.size()] == '=')
      token = item.substr(kCookie.size() + 1);
  }
  if (token.empty()) return nullptr;
  const auto now = Clock::now();
  std::erase_if(sessions_,
                [&](const auto& item) { return item.second.expires <= now; });
  const auto found = sessions_.find(token);
  return found == sessions_.end() ? nullptr : &found->second;
}

auto Dashboard::locked_seconds() -> uint64_t {
  const auto now = Clock::now();
  if (now >= locked_until_) return 0;
  return static_cast<uint64_t>(
      std::chrono::ceil<std::chrono::seconds>(locked_until_ - now).count());
}

auto Dashboard::verify_password(std::string_view given)
    -> std::optional<HTTP::Reply> {
  if (const auto wait = locked_seconds()) {
    PasswordErrorJson result;
    *result.error = "locked";
    *result.locked_seconds = wait;
    return Json(schema::encode(result), 429);
  }
  const auto stored = fs::read_file(root_ / "admin-password");
  if (!stored) return ErrorReply(403, "not_configured");
  std::string_view hash = *stored;
  while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r'))
    hash.remove_suffix(1);
  if (!tsugumi::password::Verify(hash, given)) {
    if (++failures_ > kFreeAttempts) {
      const auto step = std::min<size_t>(failures_ - kFreeAttempts - 1,
                                         std::size(kLockouts) - 1);
      locked_until_ = Clock::now() + kLockouts[step];
    }
    PasswordErrorJson result;
    *result.error = "wrong_password";
    *result.locked_seconds = locked_seconds();
    return Json(schema::encode(result), 401);
  }
  failures_ = 0;
  locked_until_ = {};
  return std::nullopt;
}

auto Dashboard::login(std::string_view given) -> HTTP::Reply {
  std::lock_guard guard(mutex_);
  if (auto error = verify_password(given)) return std::move(*error);
  const auto now = Clock::now();
  std::erase_if(sessions_,
                [&](const auto& item) { return item.second.expires <= now; });
  while (sessions_.size() >= kSessions)
    sessions_.erase(std::ranges::min_element(sessions_, {}, [](const auto& s) {
                      return s.second.expires;
                    }));
  const auto token = crypto::random_token(32);
  sessions_.emplace(token,
                    Session{crypto::random_token(32), now + kSessionLife});
  auto reply = Json("{}");
  reply.headers.push_back(
      "Set-Cookie: " + std::string(kCookie) + "=" + token +
      "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=" +
      std::to_string(std::chrono::seconds(kSessionLife).count()));
  return reply;
}

auto Dashboard::change_password(const Session& admin, std::string_view current,
                                std::string_view next,
                                std::string_view confirmation) -> HTTP::Reply {
  // Serialize with CLI resets across the read, verification and replacement.
  const auto lock = fs::FileLock::acquire(root_ / "admin-password.lock");
  if (auto error = verify_password(current)) return std::move(*error);
  if (next.size() < 8 || next.size() > 1024)
    return ErrorReply(400, "password_length");
  if (next != confirmation) return ErrorReply(400, "password_mismatch");
  fs::write_atomic(root_ / "admin-password",
                   tsugumi::password::Hash(next) + "\n", 0600);
  std::erase_if(sessions_,
                [&](const auto& item) { return &item.second != &admin; });
  return Json("{}");
}

auto Dashboard::measured() -> Measured {
  std::lock_guard guard(disk_mutex_);
  // The page refreshes more often than the store is walked again.
  if (!walking_ &&
      (disk_at_ == Clock::time_point{} || Clock::now() - disk_at_ > 60s)) {
    if (walker_.joinable()) walker_.join();  // finished: walking_ is false
    walking_ = true;
    disk_at_ = Clock::now();
    walker_ = std::thread([this] {
      Measured next{DiskBytes(root_ / "store", stopping_),
                    DiskBytes(root_ / "projects", stopping_), true};
      std::lock_guard guard(disk_mutex_);
      if (!stopping_) disk_ = next;
      disk_at_ = Clock::now();
      walking_ = false;
    });
  }
  return disk_;
}

auto Dashboard::viewer(const HTTP::Request& request) -> Viewer {
  std::lock_guard guard(mutex_);
  const auto* admin = session(request);
  return {admin != nullptr, admin ? admin->csrf : "", locked_seconds()};
}

auto Dashboard::state(const Viewer& viewer) -> std::string {
  StateJson result;
  *result.role = viewer.admin ? "admin" : "guest";
  *result.csrf = viewer.csrf;
  *result.login->configured =
      std::filesystem::exists(root_ / "admin-password");
  *result.login->locked_seconds = viewer.locked_seconds;
  const auto saved = settings_.saved();
  const auto& started = settings_.started();
  auto& worker = *result.worker;
  *worker.name = saved.name.empty() ? HostName() : saved.name;
  *worker.version = TSUGUMI_WORKERD_VERSION;
  *worker.contract = kContract;
  *worker.revision = listener_.revision;
  *worker.system = listener_.system;
  *worker.uptime_seconds = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - began_)
          .count());
  const auto bracket = started.listen_address.find(':') != std::string::npos;
  *worker.listen = "tls://" + std::string(bracket ? "[" : "") +
                   started.listen_address + (bracket ? "]" : "") + ":" +
                   std::to_string(port_);
  *worker.dashboard_port = dashboard_port_;
  *worker.running = listener_.running();
  *worker.restart_pending = listener_.restart_pending();
  *worker.restart_required = settings_.restart_required();
  for (auto& row : job_rows(root_)) {
    JobJson job;
    *job.project_scope = std::move(row.project_scope);
    *job.job_id = std::move(row.job_id);
    *job.state = std::move(row.state);
    *job.bytes = row.bytes;
    *job.last_written = std::move(row.last_written);
    *job.reason = std::move(row.reason);
    result.jobs->push_back(std::move(job));
  }
  const auto walked = measured();
  *result.disk->measured = walked.done;
  *result.disk->store_bytes = walked.store;
  *result.disk->jobs_bytes = walked.jobs;
  struct statvfs fs{};
  if (::statvfs(root_.c_str(), &fs) == 0) {
    *result.disk->free_bytes = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
    *result.disk->total_bytes =
        static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
  }
  auto& settings = *result.settings;
  *settings.name = saved.name;
  *settings.listen_address = saved.listen_address;
  *settings.port = saved.port;
  *settings.dashboard_port = saved.dashboard_port;
  *settings.max_jobs = saved.max_jobs;
  *settings.nix_max_jobs = saved.nix_max_jobs;
  *settings.nix_cores = saved.nix_cores;
  for (const auto& cache : saved.pull_caches) {
    PullJson row;
    *row.url = cache->url;
    *row.public_key = cache->public_key;
    settings.pull_caches->push_back(std::move(row));
  }
  const auto seen = listener_.last_seen();
  const auto file = peer_tls::ReadAuthorizedKeys(keys_);
  std::set<std::string> listed;
  for (const auto& line : file.lines) {
    if (!listed.insert(line.key).second) continue;
    HostJson host;
    *host.key = line.key;
    *host.comment = Comment(
        std::string_view(file.text).substr(line.begin, line.end - line.begin),
        line.key);
    if (const auto at = seen.find(line.key); at != seen.end())
      *host.last_seen = Utc(at->second);
    result.hosts->push_back(std::move(host));
  }
  return schema::encode(result);
}

auto Dashboard::edit_key(std::string_view key, bool add) -> void {
  peer_tls::ValidateKey(key);
  auto lock_path = keys_;
  lock_path += ".lock";
  struct stat st{};
  struct Lock {
    int fd;
    ~Lock() {
      if (fd >= 0) ::close(fd);
    }
  } lock{::open(lock_path.c_str(),
                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)};
  if (lock.fd < 0 || ::fstat(lock.fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & 0022))
    throw std::runtime_error("Cannot open authorized_keys update lock");
  int locked;
  do {
    locked = ::flock(lock.fd, LOCK_EX);
  } while (locked != 0 && errno == EINTR);
  if (locked != 0) throw std::runtime_error("Cannot lock authorized_keys");

  auto file = peer_tls::ReadAuthorizedKeys(keys_);
  const auto exists = std::ranges::any_of(
      file.lines, [&](const auto& line) { return line.key == key; });
  if (exists == add) return;
  auto& text = file.text;
  if (add) {
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += key;
    text += '\n';
  } else {
    // Reverse traversal keeps each remaining byte offset valid.
    for (auto i = file.lines.rbegin(); i != file.lines.rend(); ++i)
      if (i->key == key) text.erase(i->begin, i->end - i->begin);
  }
  if (text.size() > peer_tls::kAuthorizedKeysLimit)
    throw std::runtime_error("authorized_keys exceeds 1 MiB");
  fs::write_atomic(keys_, text, 0600);
}

auto Dashboard::handle(const HTTP::Request& request) -> HTTP::Reply {
  const auto question = request.path.find('?');
  const auto path = std::string_view(request.path).substr(0, question);
  const auto query = question == std::string::npos
                         ? std::string_view{}
                         : std::string_view(request.path).substr(question + 1);
  if (request.method == "GET" && request.body.empty()) {
    if (path == "/") {
      if (query == "lang=ja") return Page(true);
      if (query == "lang=en") return Page(false);
      return Page(Header(request, "accept-language").starts_with("ja"));
    }
    if (path == "/api/state") return Json(state(viewer(request)));
    return ErrorReply(404, "Unknown dashboard resource");
  }
  // Changes come only from this page: a JSON body, which a cross-site form
  // cannot send without CORS (this listener grants none), from the page's own
  // origin, and for an administrator the session's CSRF token besides the
  // SameSite cookie.
  const auto origin = Header(request, "origin");
  const auto site = Header(request, "sec-fetch-site");
  if (request.method != "POST" ||
      Header(request, "content-type") != "application/json" ||
      request.body.empty() ||
      (!origin.empty() &&
       origin != "https://" + std::string(Header(request, "host"))) ||
      (!site.empty() && site != "same-origin"))
    return ErrorReply(403, "Changes must come from the dashboard page");
  schema::Json input;
  if (schema::decode(request.body, input))
    return ErrorReply(400, "Invalid JSON body");
  const auto& object = input.value();
  if (path == "/api/login") {
    schema::v::String password;
    if (object["/password"].decode_into(password))
      return ErrorReply(400, "A password is required");
    return login(password.value());
  }
  std::unique_lock guard(mutex_);
  const auto* admin = session(request);
  if (path == "/api/logout") {
    if (admin)
      std::erase_if(sessions_,
                    [&](const auto& item) { return &item.second == admin; });
    auto reply = Json("{}");
    reply.headers.push_back("Set-Cookie: " + std::string(kCookie) +
                            "=; Path=/; Secure; HttpOnly; SameSite=Strict; "
                            "Max-Age=0");
    return reply;
  }
  if (!admin) return ErrorReply(401, "Log in as the administrator");
  if (Header(request, "x-tsugumi-csrf") != admin->csrf)
    return ErrorReply(403, "The page's session token does not match");
  if (path == "/api/password") {
    schema::v::String current, next, confirmation;
    if (object["/current_password"].decode_into(current) ||
        object["/new_password"].decode_into(next) ||
        object["/confirm_password"].decode_into(confirmation))
      return ErrorReply(400, "Enter the current and new passwords");
    return change_password(*admin, current.value(), next.value(),
                           confirmation.value());
  }
  guard.unlock();

  if (path == "/api/jobs/remove") {
    schema::v::Array<schema::v::String> names;
    if (object["/jobs"].decode_into(names) || names.value().empty() ||
        names.value().size() > 256 ||
        !std::ranges::all_of(names.value(), JobName))
      return ErrorReply(400, "Select between 1 and 256 jobs by full ID");
    RemovalsJson result;
    // Jobs recorded as queued, running or cancelling are never removed here.
    for (const auto& [name, status, detail] :
         remove_jobs(root_, false, names.value())) {
      RemovalJson row;
      *row.job = name;
      *row.status = Status(status);
      *row.detail = detail;
      result.results->push_back(std::move(row));
    }
    return Json(schema::encode(result));
  }
  if (path == "/api/keys/add" || path == "/api/keys/remove") {
    schema::v::String key;
    if (object["/key"].decode_into(key) || key.value().empty())
      return ErrorReply(400, "A public key is required");
    edit_key(key.value(), path == "/api/keys/add");
    return Json("{}");
  }
  if (path == "/api/settings") {
    schema::Obj<SettingsJson> given;
    if (object["/settings"].decode_into(given))
      return ErrorReply(400, "Every worker setting is required");
    const auto& values = given.value();
    WS::SettingsT next;
    next.name = *values.name;
    next.listen_address = *values.listen_address;
    next.port = *values.port;
    next.dashboard_port = *values.dashboard_port;
    next.max_jobs = *values.max_jobs;
    next.nix_max_jobs = *values.nix_max_jobs;
    next.nix_cores = *values.nix_cores;
    for (const auto& row : *values.pull_caches) {
      auto cache = std::make_unique<WS::PullCacheT>();
      cache->url = *row.url;
      cache->public_key = *row.public_key;
      next.pull_caches.push_back(std::move(cache));
    }
    settings_.save(std::move(next));
    return Json(state(viewer(request)));
  }
  if (path == "/api/restart") {
    schema::v::String mode;
    if (object["/mode"].decode_into(mode) ||
        (mode.value() != "after_jobs" && mode.value() != "now"))
      return ErrorReply(400, "Choose after_jobs or now");
    listener_.restart(mode.value() == "now");
    return Json("{}");
  }
  return ErrorReply(404, "Unknown dashboard resource");
}
}  // namespace tsugumi::workerd
