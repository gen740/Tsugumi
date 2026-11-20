#include "host/web_edge.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/log.hh"

namespace tsugumi::host {
namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Every response: no framing, no type sniffing, only this origin's code.
constexpr std::string_view kSecurityHeaders =
    "Content-Security-Policy: default-src 'self'; script-src 'self' "
    "'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' "
    "data: blob:; font-src 'self' data:; connect-src 'self' ws://127.0.0.1:* "
    "ws://localhost:*; object-src "
    "'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: no-referrer\r\n";

// The module viewer (/modules/: a Card's Plotly figure or Vega-Lite chart) is
// framed by the page, always sandboxed (an opaque origin: no cookie, no RPC),
// loads nothing (the page posts it the libraries' code, which it evaluates:
// 'unsafe-eval', also for Vega's expressions) and connects nowhere.
constexpr std::string_view kModuleHeaders =
    "Content-Security-Policy: sandbox allow-scripts; default-src 'none'; "
    "script-src blob: 'unsafe-inline' 'unsafe-eval'; frame-src 'none'; "
    "style-src 'unsafe-inline'; img-src data: blob:; font-src data:; "
    "connect-src 'none'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'self'\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: SAMEORIGIN\r\n"
    "Referrer-Policy: no-referrer\r\n";

auto Mime(const std::filesystem::path& file) -> const char* {
  static const std::map<std::string, const char*> types = {
      {".wasm", "application/wasm"},
      {".html", "text/html; charset=utf-8"},
      {".js", "text/javascript; charset=utf-8"},
      {".css", "text/css; charset=utf-8"},
      {".svg", "image/svg+xml"},
      {".png", "image/png"},
      {".jpg", "image/jpeg"},
      {".json", "application/json"},
      {".map", "application/json"},
      {".icns", "image/icns"},
  };
  auto it = types.find(file.extension().string());
  return it == types.end() ? "application/octet-stream" : it->second;
}

auto Lower(std::string s) -> std::string {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

auto Trim(std::string_view s) -> std::string {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return std::string(s);
}

auto PercentDecode(std::string_view s) -> std::optional<std::string> {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '%') {
      out += s[i];
      continue;
    }
    if (i + 2 >= s.size() || !std::isxdigit(static_cast<unsigned char>(s[i + 1])) ||
        !std::isxdigit(static_cast<unsigned char>(s[i + 2])))
      return std::nullopt;
    out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
    i += 2;
  }
  if (out.find('\0') != std::string::npos) return std::nullopt;
  return out;
}

struct Request {
  std::string method;
  std::string path;   // decoded later, raw here
  std::string query;  // without '?'
  std::map<std::string, std::string> headers;
};

auto Parse(std::string_view head) -> std::optional<Request> {
  Request r;
  auto line_end = head.find("\r\n");
  const auto line = head.substr(0, line_end);
  const auto sp1 = line.find(' ');
  const auto sp2 = line.rfind(' ');
  if (sp1 == std::string_view::npos || sp2 == sp1) return std::nullopt;
  r.method = std::string(line.substr(0, sp1));
  const auto target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (target.empty() || target.front() != '/') return std::nullopt;
  const auto q = target.find('?');
  r.path = std::string(target.substr(0, q));
  if (q != std::string_view::npos) r.query = std::string(target.substr(q + 1));
  while (line_end != std::string_view::npos) {
    const auto next = head.find("\r\n", line_end + 2);
    const auto field = head.substr(line_end + 2, next == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : next - line_end - 2);
    if (const auto colon = field.find(':'); colon != std::string_view::npos)
      r.headers[Lower(Trim(field.substr(0, colon)))] = Trim(field.substr(colon + 1));
    line_end = next;
  }
  return r;
}

auto QueryValue(const std::string& query, std::string_view name) -> std::optional<std::string> {
  size_t s = 0;
  while (s <= query.size()) {
    auto e = query.find('&', s);
    auto part = std::string_view(query).substr(s, e == std::string::npos ? std::string::npos : e - s);
    if (auto eq = part.find('='); eq != std::string_view::npos && part.substr(0, eq) == name)
      return PercentDecode(part.substr(eq + 1));
    if (e == std::string::npos) break;
    s = e + 1;
  }
  return std::nullopt;
}

auto WithoutQueryValue(const std::string& query, std::string_view name) -> std::string {
  std::string out;
  size_t s = 0;
  while (s <= query.size()) {
    auto e = query.find('&', s);
    auto part = std::string_view(query).substr(s, e == std::string::npos ? std::string::npos : e - s);
    auto eq = part.find('=');
    if (!part.empty() && part.substr(0, eq) != name) {
      if (!out.empty()) out += '&';
      out += part;
    }
    if (e == std::string::npos) break;
    s = e + 1;
  }
  return out;
}

auto SendAll(int fd, std::string_view bytes) -> bool {
  while (!bytes.empty()) {
    const auto n = ::send(fd, bytes.data(), bytes.size(), kSendFlags);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes.remove_prefix(static_cast<size_t>(n));
  }
  return true;
}

auto Respond(int fd, std::string_view status, std::string_view type, std::string_view body,
             std::string_view extra = {}) -> void {
  std::string out = "HTTP/1.1 " + std::string(status) + "\r\n";
  out += kSecurityHeaders;
  out += extra;
  if (!type.empty()) out += "Content-Type: " + std::string(type) + "\r\n";
  out += "Cache-Control: no-store\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n";
  out += body;
  SendAll(fd, out);
}

// The redirect stays on this origin: "//host" or "/\\host" would be another.
auto SameOriginPath(std::string_view path) -> std::string {
  while (!path.empty() && (path.front() == '/' || path.front() == '\\')) path.remove_prefix(1);
  return "/" + std::string(path);
}

}  // namespace

auto UrlEncode(std::string_view value) -> std::string {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

WebEdge::WebEdge(WebEdgeOptions options, TransportServer* host, TransportServer* agent)
    : options_(std::move(options)), host_(host), agent_(agent) {}

WebEdge::~WebEdge() { stop(); }

auto WebEdge::start() -> void {
  auto bind_to = [](int port) -> int {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("Web UI socket: " + std::string(std::strerror(errno)));
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 64) != 0) {
      const int err = errno;
      ::close(fd);
      errno = err;
      return -1;
    }
    return fd;
  };
  listen_fd_ = bind_to(options_.port);
  if (listen_fd_ < 0 && errno == EADDRINUSE && !options_.port_chosen) listen_fd_ = bind_to(0);
  if (listen_fd_ < 0)
    throw std::runtime_error("Web UI port " + std::to_string(options_.port) + ": " +
                             std::strerror(errno));
  sockaddr_in bound{};
  socklen_t len = sizeof bound;
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len);
  port_ = ntohs(bound.sin_port);
  int p[2];
  if (::pipe(p) != 0) throw std::runtime_error("pipe failed");
  wake_r_ = p[0];
  wake_w_ = p[1];
  thread_ = std::thread([this] { loop(); });
}

auto WebEdge::Release(int fd) -> void {
  std::lock_guard lock(connections_mutex_);
  connections_.erase(fd);
  ::close(fd);
}

auto WebEdge::stop() -> void {
  if (stopping_.exchange(true)) return;
  if (wake_w_ >= 0) (void)::write(wake_w_, "x", 1);
  if (thread_.joinable()) thread_.join();
  {
    // Wake readers blocked on their sockets, then wait for every thread:
    // they use this object.
    std::unique_lock lock(connections_mutex_);
    for (const int fd : connections_) ::shutdown(fd, SHUT_RDWR);
    drained_.wait(lock, [this] { return active_ == 0; });
  }
  for (int* fd : {&listen_fd_, &wake_r_, &wake_w_})
    if (*fd >= 0) ::close(std::exchange(*fd, -1));
}

auto WebEdge::page_url(const std::string& project) const -> std::string {
  auto url = "http://127.0.0.1:" + std::to_string(port_) + "/?key=" + options_.key;
  if (!project.empty()) url += "&project=" + UrlEncode(project);
  return url;
}

auto WebEdge::loop() -> void {
  while (!stopping_) {
    pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_r_, POLLIN, 0}};
    if (::poll(fds, 2, -1) < 0 && errno != EINTR) break;
    if (fds[1].revents) break;
    if (!(fds[0].revents & POLLIN)) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    timeval timeout{30, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    {
      // A page needs a handful of parallel loads; more waits for a slot.
      std::lock_guard lock(connections_mutex_);
      if (connections_.size() >= kMaxConnections) {
        ::close(fd);
        continue;
      }
      connections_.insert(fd);
      ++active_;
    }
    // A slow reader holds only its own thread; stop() waits for it.
    std::thread([this, fd] {
      serve(fd);
      std::lock_guard lock(connections_mutex_);
      if (--active_ == 0) drained_.notify_all();
    }).detach();
  }
}

auto WebEdge::serve(int fd) -> void {
  std::string head;
  char buf[4096];
  while (head.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(fd, buf, sizeof buf, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || head.size() > 16384) return void(Release(fd));
    head.append(buf, static_cast<size_t>(n));
  }
  const auto end = head.find("\r\n\r\n");
  // Bytes after the head: a WebSocket client's first frames go on with it.
  const auto rest = head.substr(end + 4);
  auto request = Parse(std::string_view(head).substr(0, end));
  if (!request) {
    Respond(fd, "400 Bad Request", "text/plain", "bad request");
    return void(Release(fd));
  }
  auto& r = *request;
  // Loopback names only (no DNS rebinding).
  const auto port = std::to_string(port_);
  const auto& host = r.headers["host"];
  if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
    Respond(fd, "403 Forbidden", "text/plain", "forbidden host");
    return void(Release(fd));
  }
  const auto cookie_name = "tsugumi_" + port;
  auto authorized = [&] {
    const auto& cookies = r.headers["cookie"];
    size_t s = 0;
    while (s <= cookies.size()) {
      auto e = cookies.find(';', s);
      auto c = Trim(std::string_view(cookies).substr(s, e == std::string::npos ? std::string::npos : e - s));
      if (c.rfind(cookie_name + "=", 0) == 0)
        return crypto::equal_secret(c.substr(cookie_name.size() + 1), options_.key);
      if (e == std::string::npos) break;
      s = e + 1;
    }
    return false;
  };
  // The URL printed at start: its key becomes the cookie, and leaves the
  // address bar. Only the page's own origin sends it (HttpOnly, Strict).
  if (auto key = QueryValue(r.query, "key"); key && crypto::equal_secret(*key, options_.key)) {
    auto query = WithoutQueryValue(r.query, "key");
    Respond(fd, "302 Found", "", "",
            "Set-Cookie: " + cookie_name + "=" + options_.key +
                "; HttpOnly; SameSite=Strict; Path=/\r\nLocation: " + SameOriginPath(r.path) +
                (query.empty() ? "" : "?" + query) + "\r\n");
    return void(Release(fd));
  }
  if (r.path == "/rpc/host" || r.path == "/rpc/agent") {
    const bool to_host = r.path == "/rpc/host";
    const auto& key = r.headers["sec-websocket-key"];
    const bool key_ok = key.size() == 24 && key.substr(22) == "==" &&
                        key.substr(0, 22).find_first_not_of(
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") ==
                            std::string::npos;
    if (r.method != "GET" || !authorized() || r.headers["origin"] != "http://" + host ||
        Lower(r.headers["upgrade"]) != "websocket" ||
        r.headers["sec-websocket-protocol"] != (to_host ? "tsugumi.host.v1" : "tsugumi.agent.v1") ||
        r.headers["sec-websocket-version"] != "13" || !key_ok) {
      Respond(fd, "403 Forbidden", "text/plain", "forbidden");
      return void(Release(fd));
    }
    // The edge checked the cookie and Origin; the RPC server sees only the
    // WebSocket handshake fields and authenticates its binary Hello itself.
    std::string forwarded = "GET /rpc HTTP/1.1\r\nHost: " + host +
                            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                            key + "\r\nSec-WebSocket-Protocol: " +
                            r.headers["sec-websocket-protocol"] + "\r\n\r\n" + rest;
    timeval none{0, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
    {
      // The RPC server owns it from here; stop() no longer waits on it.
      std::lock_guard lock(connections_mutex_);
      connections_.erase(fd);
    }
    (to_host ? host_ : agent_)->adopt(fd, std::move(forwarded));
    return;
  }
  if (r.path.rfind("/api/", 0) == 0) {
    if (!authorized()) {
      Respond(fd, "401 Unauthorized", "application/json; charset=utf-8",
              R"({"error":"This page has no key. Open it again from the URL (?key=...) the host printed when it started."})");
    } else if (r.method == "POST" && r.path == "/api/host/connect" &&
               r.headers["x-tsugumi"] == "1") {
      Respond(fd, "200 OK", "application/json; charset=utf-8",
              R"({"token":")" + options_.user_token + R"("})");
    } else {
      Respond(fd, "404 Not Found", "application/json; charset=utf-8", R"({"error":"not found"})");
    }
    return void(Release(fd));
  }
  if (r.method != "GET" && r.method != "HEAD") {
    Respond(fd, "405 Method Not Allowed", "text/plain", "");
    return void(Release(fd));
  }
  auto decoded = PercentDecode(r.path);
  auto relative = std::filesystem::path(decoded.value_or("/")).relative_path().lexically_normal();
  if (!decoded || (!relative.empty() && *relative.begin() == "..")) {
    Respond(fd, "403 Forbidden", "text/plain", "");
    return void(Release(fd));
  }
  auto file = options_.root / relative;
  struct stat st{};
  bool module = !relative.empty() && *relative.begin() == "modules";
  if (::stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    file = options_.root / "index.html";
    module = false;
  }
  const int in = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0 || ::fstat(in, &st) != 0) {
    if (in >= 0) ::close(in);
    Respond(fd, "404 Not Found", "text/plain", "not found");
    return void(Release(fd));
  }
  std::string out = "HTTP/1.1 200 OK\r\n";
  out += module ? kModuleHeaders : kSecurityHeaders;
  out += "Content-Type: " + std::string(Mime(file)) +
         "\r\nCache-Control: no-cache\r\nContent-Length: " + std::to_string(st.st_size) +
         "\r\nConnection: close\r\n\r\n";
  bool ok = SendAll(fd, out);
  while (ok && r.method == "GET") {
    const auto n = ::read(in, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    ok = SendAll(fd, std::string_view(buf, static_cast<size_t>(n)));
  }
  ::close(in);
  ::shutdown(fd, SHUT_WR);
  Release(fd);
}

}  // namespace tsugumi::host
