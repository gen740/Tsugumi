#include "dashboard.hh"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <set>
#include <stdexcept>
#include <utility>

#include "dashboard_page.hh"
#include "jobs_command.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json.hh"

namespace tsugumi::workerd {
namespace {
namespace HTTP = dashboard_http;
using namespace std::chrono_literals;

struct ErrorJson {
  schema::t::String<"error"> error;
};
struct TokenJson {
  schema::t::String<"token"> token;
};
struct KeysJson {
  schema::t::Array<"keys", schema::v::String> keys;
  schema::t::String<"current"> current;
};
struct RemovalJson {
  schema::t::String<"job"> job;
  schema::t::String<"status"> status;
  schema::t::String<"detail"> detail;
};
struct RemovalsJson {
  schema::t::Array<"results", schema::Obj<RemovalJson>> results;
};

auto Json(std::string body, unsigned status = 200) -> HTTP::Reply {
  return {status, std::string(HTTP::kJson), std::move(body)};
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
  return {200, std::string(HTTP::kHtml), std::move(html)};
}
}  // namespace

Dashboard::Dashboard(std::filesystem::path root, std::filesystem::path keys,
                     std::string server)
    : root_(std::move(root)),
      keys_(std::move(keys)),
      server_(std::move(server)) {}

auto Dashboard::serve(peer_tls::Stream& stream, std::string prefix) -> void {
  std::optional<HTTP::Request> request;
  try {
    request = HTTP::ReadRequest(stream, std::move(prefix));
  } catch (const std::exception&) {
    HTTP::WriteReply(stream, ErrorReply(400, "Invalid dashboard request"));
    return;
  }
  HTTP::Reply reply;
  try {
    reply = handle(std::move(*request), stream.peer_key());
    if (reply.body.size() > HTTP::kReplyLimit)
      reply = ErrorReply(500, "Dashboard reply is too large");
  } catch (const std::exception& e) {
    reply = ErrorReply(409, e.what());
  }
  HTTP::WriteReply(stream, reply);
}

auto Dashboard::issue(std::string peer) -> std::string {
  const auto token = crypto::random_token(32);
  std::lock_guard guard(challenge_mutex_);
  const auto now = Clock::now();
  std::erase_if(challenges_,
                [&](const auto& item) { return item.second.expires <= now; });
  if (challenges_.size() >= 1024)
    throw std::runtime_error("Too many pending dashboard requests");
  challenges_.emplace(token, Challenge{std::move(peer), now + 60s});
  return token;
}

auto Dashboard::consume(const std::string& token, std::string_view peer)
    -> bool {
  std::lock_guard guard(challenge_mutex_);
  const auto found = challenges_.find(token);
  if (found == challenges_.end() || found->second.peer != peer) return false;
  return challenges_.extract(found).mapped().expires > Clock::now();
}

auto Dashboard::edit_key(std::string_view key, bool add) -> void {
  peer_tls::ValidateKey(key);
  const auto parent = keys_.has_parent_path() ? keys_.parent_path()
                                              : std::filesystem::path(".");
  struct stat st{};
  if (::lstat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & 0022))
    throw std::runtime_error(
        "Key updates require an owned authorized_keys "
        "directory writable only by its owner");
  auto lock_path = keys_;
  lock_path += ".lock";
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

auto Dashboard::handle(HTTP::Request request, std::string peer) -> HTTP::Reply {
  auto& h = request.headers;
  const auto fetch = h.lower_bound("sec-fetch-");
  const bool browser =
      h.contains("origin") ||
      (fetch != h.end() && fetch->first.starts_with("sec-fetch-"));
  if (const auto expected = h.find("x-tsugumi-worker");
      expected != h.end() && expected->second != server_)
    return ErrorReply(409, "Worker identity changed; verify it again");
  if (request.method == "GET" && request.body.empty()) {
    if (request.path == "/" || request.path == "/dashboard" ||
        request.path == "/dashboard?lang=en")
      return Page(false);
    if (request.path == "/dashboard?lang=ja") return Page(true);
    if (request.path == "/dashboard/jobs") return Json(jobs_json(root_, false));
    if (request.path == "/dashboard/keys") {
      KeysJson result;
      *result.current = std::move(peer);
      std::set<std::string> seen;
      for (const auto& line : peer_tls::ReadAuthorizedKeys(keys_).lines)
        if (seen.insert(line.key).second) result.keys->push_back(line.key);
      return Json(schema::encode(result));
    }
    if (request.path == "/dashboard/challenge") {
      if (browser)
        return ErrorReply(403, "Dashboard challenge requires the host relay");
      TokenJson result;
      *result.token = issue(std::move(peer));
      return Json(schema::encode(result));
    }
    return ErrorReply(404, "Unknown dashboard resource");
  }
  // A page cannot send this as a simple cross-origin request and this listener
  // enables no CORS. All browser traffic goes through the isolated host bridge.
  if (request.method != "POST" || request.body.empty() ||
      h["content-type"] != "application/json" ||
      h["x-tsugumi-dashboard"] != "1" || h.contains("origin") ||
      h.contains("sec-fetch-mode"))
    return ErrorReply(403,
                      "Dashboard changes require a host relay and JSON body");
  const bool jobs = request.path == "/dashboard/jobs/remove";
  const bool add_key = request.path == "/dashboard/keys/add";
  if (!jobs && !add_key && request.path != "/dashboard/keys/remove")
    return ErrorReply(404, "Unknown dashboard resource");

  // JSON object members are unordered. Decode fields from the validated DOM
  // so an explicit approval cannot depend on their order or spelling.
  schema::Json input;
  schema::v::String token;
  if (schema::decode(request.body, input) ||
      input.value()["/token"].decode_into(token) || token.value().empty())
    return ErrorReply(400, "A dashboard challenge is required");
  const auto& object = input.value();
  if (jobs) {
    schema::v::Array<schema::v::String> names;
    schema::v::Bool active;
    if (object["/jobs"].decode_into(names) || names.value().empty() ||
        names.value().size() > 256 ||
        !std::ranges::all_of(names.value(), JobName))
      return ErrorReply(400, "Select between 1 and 256 jobs by full ID");
    if (object["/active"].decode_into(active))
      return ErrorReply(400, "An explicit active jobs choice is required");
    if (!consume(token.value(), peer))
      return ErrorReply(403, "Dashboard challenge expired or was already used");
    RemovalsJson result;
    for (const auto& [name, status, detail] :
         remove_jobs(root_, active.value(), names.value())) {
      RemovalJson row;
      *row.job = name;
      *row.status = Status(status);
      *row.detail = detail;
      result.results->push_back(std::move(row));
    }
    return Json(schema::encode(result));
  }
  schema::v::String key;
  if (object["/key"].decode_into(key) || key.value().empty())
    return ErrorReply(400, "A public key is required");
  if (!consume(token.value(), peer))
    return ErrorReply(403, "Dashboard challenge expired or was already used");
  edit_key(key.value(), add_key);
  return Json("{}");
}
}  // namespace tsugumi::workerd
