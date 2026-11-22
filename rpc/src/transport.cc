#include "tsugumi/rpc/transport.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "tsugumi/rpc/unix_socket.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json.hh"
#include "tsugumi/util/log.hh"

#ifdef MSG_NOSIGNAL
#define MSG_NOSIGNAL_FLAG MSG_NOSIGNAL
#else
#define MSG_NOSIGNAL_FLAG 0
#endif

namespace tsugumi {

namespace {

void set_nonblock(int fd) {
  int fl = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

}  // namespace

struct TransportServer::Command {
  enum class Kind { Reply, Action, Close };
  explicit Command(Kind k) : kind(k) {}
  Kind kind;
  ConnId conn = 0;
  std::string bytes;
  std::function<void()> action;
};

struct TransportServer::Conn {
  ConnId id;
  int fd;
  bool websocket;
  bool ws_open = false;  // handshake done
  bool authenticated = false;
  bool hello_seen = false;
  bool closing = false;            // close now
  bool close_after_flush = false;  // close once the outbox drained
  std::string rbuf;
  std::string ws_fragments;
  bool ws_fragmenting = false;
  // Output: raw replies and lazily built notes in one ordered queue.
  struct Out {
    std::string raw;
    std::optional<PendingMessage> note;
    bool trace = true;
  };
  std::deque<Out> outq;
  size_t out_bytes = 0;
  std::string wbuf;
  size_t wpos = 0;
  std::map<uint64_t, bool> inflight;
  const char* peer;
  trace::Link trace_link;
};

TransportServer::TransportServer(ServerOptions opts, RequestSink* sink)
    : opts_(std::move(opts)), sink_(sink) {}

TransportServer::~TransportServer() { stop(); }

void TransportServer::start() {
  int p[2];
  if (::pipe(p) != 0) throw std::runtime_error("pipe failed");
  wake_r_ = p[0];
  wake_w_ = p[1];
  set_nonblock(wake_r_);
  set_nonblock(wake_w_);
  // The caller holds the project lock, so a leftover socket file is ours to
  // replace.
  auto listen_unix = [](const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      throw std::runtime_error("create unix socket: " +
                               std::string(std::strerror(errno)));
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    sockaddr_un addr{};
    try {
      addr = unix_socket::Address(path);
    } catch (...) {
      ::close(fd);
      throw;
    }
    ::unlink(path.c_str());
    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    if (rc != 0) {
      int err = errno;
      ::close(fd);
      throw std::runtime_error("bind unix socket: " +
                               std::string(std::strerror(err)));
    }
    ::chmod(path.c_str(), 0600);
    if (::listen(fd, 64) != 0) {
      int err = errno;
      ::close(fd);
      throw std::runtime_error("listen unix socket: " +
                               std::string(std::strerror(err)));
    }
    set_nonblock(fd);
    return fd;
  };
  if (!opts_.unix_path.empty()) unix_fd_ = listen_unix(opts_.unix_path);
  if (!opts_.ws_unix_path.empty())
    ws_unix_fd_ = listen_unix(opts_.ws_unix_path);
  if (opts_.tcp_port >= 0) {
    tcp_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(tcp_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(opts_.tcp_port));
    if (::inet_pton(AF_INET, opts_.tcp_host.c_str(), &addr.sin_addr) != 1)
      throw std::runtime_error("invalid listen address " + opts_.tcp_host);
    if (::bind(tcp_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
      throw std::runtime_error("bind tcp: " +
                               std::string(std::strerror(errno)));
    ::listen(tcp_fd_, 64);
    socklen_t len = sizeof addr;
    ::getsockname(tcp_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    bound_port_ = ntohs(addr.sin_port);
    set_nonblock(tcp_fd_);
  }
  thread_ = std::thread([this] { loop(); });
}

void TransportServer::stop() {
  if (stopping_.exchange(true)) return;
  wake();
  if (thread_.joinable()) thread_.join();
  for (auto& [_, c] : conns_) ::close(c->fd);
  conns_.clear();
  for (int fd : {unix_fd_, ws_unix_fd_, tcp_fd_, wake_r_, wake_w_})
    if (fd >= 0) ::close(fd);
  if (!opts_.unix_path.empty()) ::unlink(opts_.unix_path.c_str());
  if (!opts_.ws_unix_path.empty()) ::unlink(opts_.ws_unix_path.c_str());
}

void TransportServer::wake() {
  char b = 1;
  if (wake_w_ < 0) return;
  ssize_t count;
  do {
    count = ::write(wake_w_, &b, 1);
  } while (count < 0 && errno == EINTR);
  // A full nonblocking pipe already wakes the loop; notifications coalesce.
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
  if (count != 1)
    log::warn("cannot wake server: " +
              std::string(std::strerror(count < 0 ? errno : EIO)));
}

void TransportServer::post(Command c) {
  {
    std::lock_guard<std::mutex> l(inbox_mu_);
    inbox_.push_back(std::move(c));
  }
  wake();
}

void TransportServer::reply(ConnId conn, std::string bytes) {
  Command c{Command::Kind::Reply};
  c.conn = conn;
  c.bytes = std::move(bytes);
  post(std::move(c));
}

void TransportServer::ordered(std::function<void()> action) {
  Command command{Command::Kind::Action};
  command.action = std::move(action);
  post(std::move(command));
}
void TransportServer::ordered(ConnId conn, std::function<void()> action) {
  ordered([this, conn, action = std::move(action)] {
    if (conns_.contains(conn)) action();
  });
}
void TransportServer::send(ConnId conn, std::string bytes) {
  if (auto it = conns_.find(conn); it != conns_.end())
    enqueue_raw(*it->second, std::move(bytes), true);
}
void TransportServer::send_note(ConnId conn, PendingMessage note) {
  if (auto it = conns_.find(conn); it != conns_.end())
    enqueue_note(*it->second, note);
}
void TransportServer::clear_notes(ConnId conn) {
  auto it = conns_.find(conn);
  if (it == conns_.end()) return;
  auto& c = *it->second;
  for (auto o = c.outq.begin(); o != c.outq.end();) {
    if (o->note) {
      c.out_bytes -= o->note->approx_bytes;
      o = c.outq.erase(o);
    } else
      ++o;
  }
}

void TransportServer::close(ConnId conn) {
  Command c{Command::Kind::Close};
  c.conn = conn;
  post(std::move(c));
}

void TransportServer::apply(Command& cmd) {
  if (cmd.kind == Command::Kind::Action) {
    cmd.action();
    return;
  }
  auto it = conns_.find(cmd.conn);
  if (it == conns_.end()) return;
  if (cmd.kind == Command::Kind::Reply)
    enqueue_raw(*it->second, std::move(cmd.bytes), true);
  else
    drop(*it->second, "closed by server");
}

void TransportServer::enqueue_raw(Conn& c, std::string bytes, bool is_reply,
                                  bool trace) {
  if (c.closing) return;
  if (is_reply) {
    auto request = c.inflight.extract(opts_.codec.completed(bytes));
    if (!request.empty()) trace = request.mapped();
  }
  c.out_bytes += bytes.size();
  Conn::Out o;
  o.raw = std::move(bytes);
  o.trace = trace;
  c.outq.push_back(std::move(o));
}

void TransportServer::enqueue_note(Conn& c, const PendingMessage& n) {
  if (c.closing) return;
  if (!n.coalesce_key.empty()) {
    for (auto& o : c.outq) {
      if (o.note && o.note->group == n.group &&
          o.note->coalesce_key == n.coalesce_key) {
        c.out_bytes -= o.note->approx_bytes;
        o.note = n;
        c.out_bytes += n.approx_bytes;
        return;
      }
    }
  }
  Conn::Out o;
  o.note = n;
  c.out_bytes += n.approx_bytes;
  c.outq.push_back(std::move(o));
}

// Moves the next queued item into the write buffer. Returns false when empty.
bool TransportServer::fill_write(Conn& c) {
  if (c.wpos < c.wbuf.size()) return true;
  c.wbuf.clear();
  c.wpos = 0;
  if (c.outq.empty()) return false;
  Conn::Out o = std::move(c.outq.front());
  c.outq.pop_front();
  std::string body;
  if (o.note) {
    c.out_bytes -= o.note->approx_bytes;
    body = o.note->build();
  } else {
    c.out_bytes -= o.raw.size();
    body = std::move(o.raw);
  }
  trace::Observe(c.trace_link, opts_.trace_endpoint, c.peer, true, body,
                 opts_.trace_codec, o.note ? o.note->trace : o.trace);
  if (c.websocket) {
    std::string h;
    h += static_cast<char>(0x82);  // FIN + binary
    if (body.size() < 126) {
      h += static_cast<char>(body.size());
    } else if (body.size() <= 0xFFFF) {
      h += static_cast<char>(126);
      h += static_cast<char>(body.size() >> 8);
      h += static_cast<char>(body.size() & 0xFF);
    } else {
      h += static_cast<char>(127);
      for (int i = 7; i >= 0; --i)
        h += static_cast<char>((uint64_t(body.size()) >> (i * 8)) & 0xFF);
    }
    c.wbuf = h + body;
  } else {
    uint32_t len = static_cast<uint32_t>(body.size());
    c.wbuf.assign(reinterpret_cast<const char*>(&len), 4);
    c.wbuf += body;
  }
  return true;
}

void TransportServer::on_writable(Conn& c) {
  while (fill_write(c)) {
    ssize_t w = ::send(c.fd, c.wbuf.data() + c.wpos, c.wbuf.size() - c.wpos,
                       MSG_NOSIGNAL_FLAG);
    if (w < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      drop(c, "write error");
      return;
    }
    c.wpos += static_cast<size_t>(w);
  }
}

void TransportServer::drop(Conn& c, const char* why) {
  if (c.closing && c.fd < 0) return;
  log::info("connection " + std::to_string(c.id) + " closed: " + why);
  c.closing = true;
  c.outq.clear();
  c.wbuf.clear();
  c.wpos = 0;
}

void TransportServer::accept_on(int lfd, bool websocket) {
  while (true) {
    int fd = ::accept(lfd, nullptr, nullptr);
    if (fd < 0) return;
    set_nonblock(fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (websocket) {
      int one2 = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one2, sizeof one2);
    }
    auto c = std::make_unique<Conn>();
    c->id = next_id_++;
    c->fd = fd;
    c->websocket = websocket;
    c->peer = websocket ? "WebSocket client" : "Unix client";
    conns_[c->id] = std::move(c);
  }
}

void TransportServer::adopt(int fd, std::string head) {
  Command c(Command::Kind::Action);
  c.action = [this, fd, head = std::move(head)]() mutable {
    set_nonblock(fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
    auto conn = std::make_unique<Conn>();
    conn->id = next_id_++;
    conn->fd = fd;
    conn->websocket = true;
    conn->peer = "WebSocket client";
    conn->rbuf = std::move(head);
    auto& c = *conn;
    conns_[c.id] = std::move(conn);
    if (handle_ws_handshake(c)) parse_ws(c);
  };
  post(std::move(c));
}

bool TransportServer::handle_ws_handshake(Conn& c) {
  auto end = c.rbuf.find("\r\n\r\n");
  if (end == std::string::npos) {
    if (c.rbuf.size() > 16384) drop(c, "oversized handshake");
    return false;
  }
  std::string head = c.rbuf.substr(0, end);
  c.rbuf.erase(0, end + 4);
  std::map<std::string, std::string> h;
  size_t pos = head.find("\r\n");
  std::string request_line = head.substr(0, pos);
  while (pos != std::string::npos) {
    size_t next = head.find("\r\n", pos + 2);
    std::string line =
        head.substr(pos + 2, next == std::string::npos ? std::string::npos
                                                       : next - pos - 2);
    auto colon = line.find(':');
    if (colon != std::string::npos)
      h[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    pos = next;
  }
  auto reject = [&](const char* status) {
    std::string resp = std::string("HTTP/1.1 ") + status +
                       "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    (void)::send(c.fd, resp.data(), resp.size(), MSG_NOSIGNAL_FLAG);
    drop(c, status);
    return false;
  };
  if (request_line.rfind("GET /rpc ", 0) != 0) return reject("404 Not Found");
  if (lower(h["upgrade"]) != "websocket" || h["sec-websocket-key"].empty() ||
      h["sec-websocket-version"] != "13")
    return reject("400 Bad Request");
  // Subprotocol must be offered.
  bool proto = false;
  {
    std::string list = h["sec-websocket-protocol"];
    size_t s = 0;
    while (s <= list.size()) {
      size_t e = list.find(',', s);
      if (trim(list.substr(
              s, e == std::string::npos ? std::string::npos : e - s)) ==
          opts_.subprotocol)
        proto = true;
      if (e == std::string::npos) break;
      s = e + 1;
    }
  }
  if (!proto) return reject("400 Bad Request");
  auto origin = h.find("origin");
  if (origin == h.end()) {
    if (!opts_.allow_missing_origin) return reject("403 Forbidden");
  } else {
    bool allowed =
        std::find(opts_.allowed_origins.begin(), opts_.allowed_origins.end(),
                  origin->second) != opts_.allowed_origins.end();
    if (!allowed && !opts_.origins_file.empty()) {
      struct OriginList {
        schema::t::Array<"origins", schema::v::String> origins;
      };
      OriginList list;
      if (auto bytes = fs::read_file(opts_.origins_file);
          bytes && !schema::decode(std::string_view(*bytes), list)) {
        const auto& origins = *list.origins;
        allowed = std::find(origins.begin(), origins.end(), origin->second) !=
                  origins.end();
      }
    }
    if (!allowed) return reject("403 Forbidden");
  }
  auto digest = crypto::sha1(h["sec-websocket-key"] +
                             "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  std::string resp =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
      "Upgrade\r\n"
      "Sec-WebSocket-Accept: " +
      crypto::base64(digest.data(), digest.size()) +
      "\r\nSec-WebSocket-Protocol: " + opts_.subprotocol + "\r\n\r\n";
  c.wbuf = resp;  // first bytes out, before any frame
  c.wpos = 0;
  c.ws_open = true;
  return true;
}

void TransportServer::parse_ws(Conn& c) {
  size_t consumed = 0;
  while (!c.closing) {
    const auto b = std::string_view(c.rbuf).substr(consumed);
    if (b.size() < 2) break;
    auto b0 = static_cast<uint8_t>(b[0]);
    auto b1 = static_cast<uint8_t>(b[1]);
    bool fin = b0 & 0x80;
    int opcode = b0 & 0x0F;
    uint64_t len = b1 & 0x7F;
    size_t off = 2;
    if (len == 126) {
      if (b.size() < 4) break;
      len = (uint64_t(uint8_t(b[2])) << 8) | uint8_t(b[3]);
      off = 4;
    } else if (len == 127) {
      if (b.size() < 10) break;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(b[2 + i]);
      off = 10;
    }
    if (len > opts_.limits.max_frame_bytes ||
        c.ws_fragments.size() + len > opts_.limits.max_frame_bytes)
      return drop(c, "frame too large");
    if (b.size() < off + 4 + len) break;
    const char* mask = b.data() + off;
    std::string payload(b.substr(off + 4, len));
    for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    consumed += off + 4 + len;
    if (opcode >= 0x8) {
      if (opcode == 0x8) {
        std::string close_frame = "\x88";
        close_frame += static_cast<char>(0);
        (void)::send(c.fd, close_frame.data(), close_frame.size(),
                     MSG_NOSIGNAL_FLAG);
        return drop(c, "websocket close");
      }
      if (opcode == 0x9) {
        std::string pong;
        pong += static_cast<char>(0x8A);
        pong += static_cast<char>(payload.size());
        pong += payload;
        // Control frames may be interleaved; send directly when idle.
        if (c.wpos >= c.wbuf.size()) {
          c.wbuf = pong;
          c.wpos = 0;
        }
      }
      continue;
    }
    if (opcode == 0x1) return drop(c, "text frames are not accepted");
    if (opcode == 0x2) {
      if (c.ws_fragmenting)
        return drop(c, "unexpected new message during fragmentation");
      if (fin) {
        handle_frame(c, std::move(payload));
      } else {
        c.ws_fragmenting = true;
        c.ws_fragments = std::move(payload);
      }
    } else if (opcode == 0x0) {
      if (!c.ws_fragmenting) return drop(c, "unexpected continuation");
      c.ws_fragments += payload;
      if (fin) {
        c.ws_fragmenting = false;
        handle_frame(c, std::move(c.ws_fragments));
        c.ws_fragments.clear();
      }
    } else {
      return drop(c, "unknown opcode");
    }
  }
  c.rbuf.erase(0, consumed);
}

void TransportServer::on_readable(Conn& c) {
  char buf[65536];
  bool eof = false;
  while (!c.closing) {
    ssize_t r = ::recv(c.fd, buf, sizeof buf, 0);
    if (r == 0) {
      eof = true;
      break;
    }
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      return drop(c, "read error");
    }
    c.rbuf.append(buf, static_cast<size_t>(r));
  }
  // Requests that arrived before the peer closed are still accepted.
  if (c.websocket) {
    if (c.ws_open || handle_ws_handshake(c)) parse_ws(c);
  } else {
    size_t consumed = 0;
    while (!c.closing && c.rbuf.size() - consumed >= 4) {
      uint32_t len;
      std::memcpy(&len, c.rbuf.data() + consumed, 4);
      if (len == 0 || len > opts_.limits.max_frame_bytes)
        return drop(c, "invalid frame length");
      if (c.rbuf.size() - consumed < 4 + size_t(len)) break;
      std::string frame = c.rbuf.substr(consumed + 4, len);
      consumed += 4 + size_t(len);
      handle_frame(c, std::move(frame));
    }
    c.rbuf.erase(0, consumed);
  }
  if (eof) drop(c, "peer closed");
}

void TransportServer::handle_frame(Conn& c, std::string frame) {
  const auto info = opts_.codec.inspect(frame, opts_.limits.max_frame_bytes);
  if (!info) return drop(c, "invalid request frame");
  const auto rid = info->request_id;
  if (c.inflight.count(rid)) return drop(c, "duplicate in-flight request_id");
  const auto fail = [&](AdmissionFailure reason) {
    enqueue_raw(c, opts_.codec.failure(rid, reason), false, info->trace);
  };
  RequestIn in{c.id, rid, std::make_shared<const std::string>(std::move(frame)),
               c.websocket};
  if (info->hello) {
    if (!c.hello_seen && info->peer) c.peer = info->peer;
    trace::Observe(c.trace_link, c.peer, opts_.trace_endpoint, false, *in.frame,
                   opts_.trace_codec, info->trace);
    if (c.hello_seen) return fail(AdmissionFailure::AlreadyHello);
    c.hello_seen = true;
    auto [bytes, ok] = sink_->hello(in);
    c.authenticated = ok;
    enqueue_raw(c, std::move(bytes), false, info->trace);
    if (!ok) c.close_after_flush = true;
    return;
  }
  trace::Observe(c.trace_link, c.peer, opts_.trace_endpoint, false, *in.frame,
                 opts_.trace_codec, info->trace);
  if (!c.authenticated) return fail(AdmissionFailure::Unauthenticated);
  c.inflight.emplace(rid, info->trace);
  sink_->dispatch(std::move(in));
}

void TransportServer::loop() {
  while (!stopping_) {
    {
      std::vector<Command> cmds;
      {
        std::lock_guard<std::mutex> l(inbox_mu_);
        cmds.swap(inbox_);
      }
      for (auto& c : cmds) apply(c);
    }
    std::vector<pollfd> fds;
    std::vector<ConnId> ids;
    fds.push_back({wake_r_, POLLIN, 0});
    ids.push_back(0);
    if (unix_fd_ >= 0) {
      fds.push_back({unix_fd_, POLLIN, 0});
      ids.push_back(0);
    }
    if (ws_unix_fd_ >= 0) {
      fds.push_back({ws_unix_fd_, POLLIN, 0});
      ids.push_back(0);
    }
    if (tcp_fd_ >= 0) {
      fds.push_back({tcp_fd_, POLLIN, 0});
      ids.push_back(0);
    }
    for (auto it = conns_.begin(); it != conns_.end();) {
      Conn& c = *it->second;
      if (c.close_after_flush && c.outq.empty() && c.wpos >= c.wbuf.size())
        c.closing = true;
      if (c.closing && c.wpos >= c.wbuf.size()) {
        ::close(c.fd);
        ConnId id = c.id;
        it = conns_.erase(it);
        sink_->disconnected(id);
        continue;
      }
      short ev = POLLIN;
      if (c.wpos < c.wbuf.size() || !c.outq.empty()) ev |= POLLOUT;
      fds.push_back({c.fd, ev, 0});
      ids.push_back(c.id);
      ++it;
    }
    int rc = ::poll(fds.data(), fds.size(), -1);
    if (rc < 0 && errno != EINTR) break;
    size_t i = 0;
    if (fds[i].revents & POLLIN) {
      char tmp[256];
      while (::read(wake_r_, tmp, sizeof tmp) > 0) {
      }
    }
    ++i;
    if (unix_fd_ >= 0) {
      if (fds[i].revents & POLLIN) accept_on(unix_fd_, false);
      ++i;
    }
    if (ws_unix_fd_ >= 0) {
      if (fds[i].revents & POLLIN) accept_on(ws_unix_fd_, true);
      ++i;
    }
    if (tcp_fd_ >= 0) {
      if (fds[i].revents & POLLIN) accept_on(tcp_fd_, true);
      ++i;
    }
    for (; i < fds.size(); ++i) {
      auto it = conns_.find(ids[i]);
      if (it == conns_.end()) continue;
      Conn& c = *it->second;
      if (fds[i].revents & (POLLERR | POLLNVAL)) {
        drop(c, "socket error");
        continue;
      }
      if (fds[i].revents & (POLLIN | POLLHUP)) on_readable(c);
      if (!c.closing || c.wpos < c.wbuf.size()) on_writable(c);
    }
  }
}

}  // namespace tsugumi
