#include "server.hh"

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
#include <stdexcept>

#include "util/crypto.hh"
#include "util/log.hh"

#ifdef MSG_NOSIGNAL
#define MSG_NOSIGNAL_FLAG MSG_NOSIGNAL
#else
#define MSG_NOSIGNAL_FLAG 0
#endif

namespace tsugumi {

namespace R = Tsugumi::RPC;

const R::Request* RequestIn::request() const {
  return R::GetMessage(frame->data())->payload_as_Request();
}

namespace {

int64_t mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void set_nonblock(int fd) {
  int fl = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

}  // namespace

struct Server::Command {
  enum class Kind { Reply, Subscribe, Unsubscribe, SetVisible, Broadcast, Close };
  explicit Command(Kind k) : kind(k) {}
  Kind kind;
  ConnId conn = 0;
  std::string bytes;
  std::string sub_id;
  std::set<std::string> cards;
  bool enabled = true;
  std::vector<Note> notes;
};

struct Server::Conn {
  ConnId id;
  int fd;
  bool websocket;
  bool ws_open = false;  // handshake done
  bool authenticated = false;
  bool hello_seen = false;
  bool closing = false;             // close now
  bool close_after_flush = false;   // close once the outbox drained
  int64_t connected_at;
  std::string rbuf;
  std::string ws_fragments;
  bool ws_fragmenting = false;
  // Output: raw replies and lazily built notes in one ordered queue.
  struct Out {
    std::string raw;
    std::optional<Note> note;
    std::string sub_id;
    bool is_reply = false;
  };
  std::deque<Out> outq;
  size_t out_bytes = 0;
  std::string wbuf;
  size_t wpos = 0;
  std::set<uint64_t> inflight;
  // Subscription.
  std::string sub_id;
  uint64_t seq = 0;
  std::set<std::string> visible;
  bool visible_enabled = true;
};

Server::Server(ServerOptions opts, RequestSink* sink) : opts_(std::move(opts)), sink_(sink) {}

Server::~Server() { stop(); }

void Server::start() {
  int p[2];
  if (::pipe(p) != 0) throw std::runtime_error("pipe failed");
  wake_r_ = p[0];
  wake_w_ = p[1];
  set_nonblock(wake_r_);
  set_nonblock(wake_w_);
  // The caller holds the project lock, so a leftover socket file is ours to replace.
  auto listen_unix = [](const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) throw std::runtime_error("unix socket path too long");
    std::strcpy(addr.sun_path, path.c_str());
    ::unlink(path.c_str());
    mode_t old = ::umask(0177);
    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::umask(old);
    if (rc != 0) throw std::runtime_error("bind unix socket: " + std::string(std::strerror(errno)));
    ::chmod(path.c_str(), 0600);
    ::listen(fd, 64);
    set_nonblock(fd);
    return fd;
  };
  if (!opts_.unix_path.empty()) unix_fd_ = listen_unix(opts_.unix_path);
  if (!opts_.ws_unix_path.empty()) ws_unix_fd_ = listen_unix(opts_.ws_unix_path);
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
      throw std::runtime_error("bind tcp: " + std::string(std::strerror(errno)));
    ::listen(tcp_fd_, 64);
    socklen_t len = sizeof addr;
    ::getsockname(tcp_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    bound_port_ = ntohs(addr.sin_port);
    set_nonblock(tcp_fd_);
  }
  thread_ = std::thread([this] { loop(); });
}

void Server::stop() {
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

void Server::wake() {
  char b = 1;
  if (wake_w_ >= 0) (void)::write(wake_w_, &b, 1);
}

void Server::post(Command c) {
  {
    std::lock_guard<std::mutex> l(inbox_mu_);
    inbox_.push_back(std::move(c));
  }
  wake();
}

void Server::reply(ConnId conn, std::string bytes) {
  Command c{Command::Kind::Reply};
  c.conn = conn;
  c.bytes = std::move(bytes);
  post(std::move(c));
}

void Server::subscribe(ConnId conn, std::string sub_id, std::string open_reply) {
  Command c{Command::Kind::Subscribe};
  c.conn = conn;
  c.sub_id = std::move(sub_id);
  c.bytes = std::move(open_reply);
  post(std::move(c));
}

void Server::unsubscribe(ConnId conn, std::string sub_id, std::string ack) {
  Command c{Command::Kind::Unsubscribe};
  c.conn = conn;
  c.sub_id = std::move(sub_id);
  c.bytes = std::move(ack);
  post(std::move(c));
}

void Server::set_visible(ConnId conn, std::string sub_id, std::set<std::string> cards, bool enabled,
                         std::string ack, std::vector<Note> initial) {
  Command c{Command::Kind::SetVisible};
  c.conn = conn;
  c.sub_id = std::move(sub_id);
  c.cards = std::move(cards);
  c.enabled = enabled;
  c.bytes = std::move(ack);
  c.notes = std::move(initial);
  post(std::move(c));
}

void Server::broadcast(Note note) {
  Command c{Command::Kind::Broadcast};
  c.notes.push_back(std::move(note));
  post(std::move(c));
}

void Server::close(ConnId conn) {
  Command c{Command::Kind::Close};
  c.conn = conn;
  post(std::move(c));
}

void Server::apply(Command& cmd) {
  if (cmd.kind == Command::Kind::Broadcast) {
    for (auto& [_, c] : conns_) {
      if (c->sub_id.empty()) continue;
      const Note& n = cmd.notes.front();
      if (n.kind == Note::Kind::Display && (!c->visible_enabled || !c->visible.count(n.card_key)))
        continue;
      enqueue_note(*c, n);
    }
    return;
  }
  auto it = conns_.find(cmd.conn);
  if (it == conns_.end()) return;  // peer already gone; delivery is not guaranteed
  Conn& c = *it->second;
  switch (cmd.kind) {
    case Command::Kind::Reply:
      enqueue_raw(c, std::move(cmd.bytes), true);
      break;
    case Command::Kind::Subscribe: {
      // Replace any previous subscription and drop its unsent notes.
      for (auto o = c.outq.begin(); o != c.outq.end();) {
        if (o->note) {
          c.out_bytes -= o->note->approx_bytes;
          o = c.outq.erase(o);
        } else {
          ++o;
        }
      }
      c.sub_id = cmd.sub_id;
      c.seq = 1;  // the Open reply carries seq 1
      c.visible.clear();
      c.visible_enabled = true;
      enqueue_raw(c, std::move(cmd.bytes), true);
      break;
    }
    case Command::Kind::Unsubscribe:
      if (c.sub_id == cmd.sub_id) {
        for (auto o = c.outq.begin(); o != c.outq.end();) {
          if (o->note && o->sub_id == cmd.sub_id) {
            c.out_bytes -= o->note->approx_bytes;
            o = c.outq.erase(o);
          } else {
            ++o;
          }
        }
        c.sub_id.clear();
        c.visible.clear();
      }
      enqueue_raw(c, std::move(cmd.bytes), true);
      break;
    case Command::Kind::SetVisible:
      if (c.sub_id == cmd.sub_id) {
        c.visible = std::move(cmd.cards);
        c.visible_enabled = cmd.enabled;
      }
      enqueue_raw(c, std::move(cmd.bytes), true);
      if (c.sub_id == cmd.sub_id && c.visible_enabled)
        for (const auto& n : cmd.notes)
          if (c.visible.count(n.card_key)) enqueue_note(c, n);
      break;
    case Command::Kind::Close:
      drop(c, "closed by server");
      break;
    default: break;
  }
}

void Server::enforce_outbox(Conn& c) {
  if (c.outq.size() <= opts_.limits.outbox_messages && c.out_bytes <= opts_.limits.outbox_bytes) return;
  // Hand what the socket accepts to the kernel first; only a peer that does
  // not read is disconnected.
  on_writable(c);
  if (c.outq.size() > opts_.limits.outbox_messages || c.out_bytes > opts_.limits.outbox_bytes)
    drop(c, "outbox limit exceeded (slow peer)");
}

void Server::enqueue_raw(Conn& c, std::string bytes, bool is_reply) {
  if (c.closing) return;
  if (is_reply && bytes.size() >= 4) {
    // Track completion of in-flight requests.
    auto* m = R::GetMessage(bytes.data());
    if (m->request_id()) c.inflight.erase(*m->request_id());
  }
  c.out_bytes += bytes.size();
  Conn::Out o;
  o.raw = std::move(bytes);
  o.is_reply = is_reply;
  c.outq.push_back(std::move(o));
  enforce_outbox(c);
}

void Server::enqueue_note(Conn& c, const Note& n) {
  if (c.closing) return;
  if (!n.coalesce_key.empty()) {
    for (auto& o : c.outq) {
      if (o.note && o.sub_id == c.sub_id && o.note->coalesce_key == n.coalesce_key) {
        c.out_bytes -= o.note->approx_bytes;
        o.note = n;
        c.out_bytes += n.approx_bytes;
        return;
      }
    }
  }
  Conn::Out o;
  o.note = n;
  o.sub_id = c.sub_id;
  c.out_bytes += n.approx_bytes;
  c.outq.push_back(std::move(o));
  enforce_outbox(c);
}

// Moves the next queued item into the write buffer. Returns false when empty.
bool Server::fill_write(Conn& c) {
  if (c.wpos < c.wbuf.size()) return true;
  c.wbuf.clear();
  c.wpos = 0;
  if (c.outq.empty()) return false;
  Conn::Out o = std::move(c.outq.front());
  c.outq.pop_front();
  std::string body;
  if (o.note) {
    c.out_bytes -= o.note->approx_bytes;
    flatbuffers::FlatBufferBuilder b(1024);
    auto [type, value] = (*o.note->build)(b);
    auto sub = b.CreateString(o.sub_id);
    uint64_t seq = ++c.seq;
    auto note = R::CreateNotification(b, sub, seq, type, value);
    R::MessageBuilder mb(b);
    mb.add_payload_type(R::Payload::Notification);
    mb.add_payload(note.Union());
    R::FinishMessageBuffer(b, mb.Finish());
    body.assign(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
  } else {
    c.out_bytes -= o.raw.size();
    body = std::move(o.raw);
  }
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
      for (int i = 7; i >= 0; --i) h += static_cast<char>((uint64_t(body.size()) >> (i * 8)) & 0xFF);
    }
    c.wbuf = h + body;
  } else {
    uint32_t len = static_cast<uint32_t>(body.size());
    c.wbuf.assign(reinterpret_cast<const char*>(&len), 4);
    c.wbuf += body;
  }
  return true;
}

void Server::on_writable(Conn& c) {
  while (fill_write(c)) {
    ssize_t w = ::send(c.fd, c.wbuf.data() + c.wpos, c.wbuf.size() - c.wpos, MSG_NOSIGNAL_FLAG);
    if (w < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      drop(c, "write error");
      return;
    }
    c.wpos += static_cast<size_t>(w);
  }
}

void Server::drop(Conn& c, const char* why) {
  if (c.closing && c.fd < 0) return;
  log::info("connection " + std::to_string(c.id) + " closed: " + why);
  c.closing = true;
  c.outq.clear();
  c.wbuf.clear();
  c.wpos = 0;
}

void Server::accept_on(int lfd, bool websocket) {
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
    } else {
      // Same user only on the Unix socket.
#if defined(__APPLE__)
      uid_t uid;
      gid_t gid;
      if (::getpeereid(fd, &uid, &gid) != 0 || uid != ::geteuid()) {
        ::close(fd);
        continue;
      }
#else
      struct ucred cred;
      socklen_t len = sizeof cred;
      if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != ::geteuid()) {
        ::close(fd);
        continue;
      }
#endif
    }
    if (conns_.size() >= opts_.limits.max_connections) {
      ::close(fd);
      continue;
    }
    auto c = std::make_unique<Conn>();
    c->id = next_id_++;
    c->fd = fd;
    c->websocket = websocket;
    c->connected_at = mono_ms();
    conns_[c->id] = std::move(c);
    conn_count_ = conns_.size();
  }
}

bool Server::handle_ws_handshake(Conn& c) {
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
    std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
    auto colon = line.find(':');
    if (colon != std::string::npos) h[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    pos = next;
  }
  auto reject = [&](const char* status) {
    std::string resp = std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
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
      if (trim(list.substr(s, e == std::string::npos ? std::string::npos : e - s)) == "tsugumi.rpc.v1")
        proto = true;
      if (e == std::string::npos) break;
      s = e + 1;
    }
  }
  if (!proto) return reject("400 Bad Request");
  auto origin = h.find("origin");
  if (origin == h.end()) {
    if (!opts_.allow_missing_origin) return reject("403 Forbidden");
  } else if (std::find(opts_.allowed_origins.begin(), opts_.allowed_origins.end(), origin->second) ==
             opts_.allowed_origins.end()) {
    return reject("403 Forbidden");
  }
  auto digest = crypto::sha1(h["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  std::string resp =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " + crypto::base64(digest.data(), digest.size()) +
      "\r\nSec-WebSocket-Protocol: tsugumi.rpc.v1\r\n\r\n";
  c.wbuf = resp;  // first bytes out, before any frame
  c.wpos = 0;
  c.ws_open = true;
  return true;
}

void Server::parse_ws(Conn& c) {
  while (!c.closing) {
    auto& b = c.rbuf;
    if (b.size() < 2) return;
    auto b0 = static_cast<uint8_t>(b[0]);
    auto b1 = static_cast<uint8_t>(b[1]);
    bool fin = b0 & 0x80;
    int opcode = b0 & 0x0F;
    if (b0 & 0x70) return drop(c, "websocket reserved bits");
    if (!(b1 & 0x80)) return drop(c, "unmasked client frame");
    uint64_t len = b1 & 0x7F;
    size_t off = 2;
    if (len == 126) {
      if (b.size() < 4) return;
      len = (uint64_t(uint8_t(b[2])) << 8) | uint8_t(b[3]);
      off = 4;
    } else if (len == 127) {
      if (b.size() < 10) return;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(b[2 + i]);
      off = 10;
    }
    if (len > opts_.limits.max_frame_bytes || c.ws_fragments.size() + len > opts_.limits.max_frame_bytes)
      return drop(c, "frame too large");
    if (b.size() < off + 4 + len) return;
    const char* mask = b.data() + off;
    std::string payload = b.substr(off + 4, len);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    b.erase(0, off + 4 + len);
    if (opcode >= 0x8) {
      if (!fin || len > 125) return drop(c, "invalid control frame");
      if (opcode == 0x8) {
        std::string close_frame = "\x88";
        close_frame += static_cast<char>(0);
        (void)::send(c.fd, close_frame.data(), close_frame.size(), MSG_NOSIGNAL_FLAG);
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
      if (c.ws_fragmenting) return drop(c, "unexpected new message during fragmentation");
      if (fin) {
        handle_frame(c, std::move(payload));
      } else {
        c.ws_fragmenting = true;
        c.ws_fragments = std::move(payload);
      }
    } else if (opcode == 0x0) {
      if (!c.ws_fragmenting) return drop(c, "unexpected continuation");
      if (c.ws_fragments.size() + payload.size() > opts_.limits.max_frame_bytes) return drop(c, "message too large");
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
}

// What a peer may send before its Hello (the WebSocket handshake included).
constexpr size_t kPreAuthBytes = 64 * 1024;

void Server::on_readable(Conn& c) {
  char buf[65536];
  bool eof = false;
  // Before Hello a peer sends a handshake and one small frame: more is
  // dropped. After it, a frame at a time is buffered (the rest waits in the
  // socket until these are handled).
  const size_t limit = c.authenticated ? size_t(opts_.limits.max_frame_bytes) + sizeof buf : kPreAuthBytes;
  while (!c.closing && c.rbuf.size() < limit) {
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
    while (!c.closing && c.rbuf.size() >= 4) {
      uint32_t len;
      std::memcpy(&len, c.rbuf.data(), 4);
      if (len == 0 || len > opts_.limits.max_frame_bytes) return drop(c, "invalid frame length");
      if (c.rbuf.size() < 4 + size_t(len)) break;
      std::string frame = c.rbuf.substr(4, len);
      c.rbuf.erase(0, 4 + size_t(len));
      handle_frame(c, std::move(frame));
    }
  }
  if (!c.authenticated && !c.closing && c.rbuf.size() >= kPreAuthBytes) return drop(c, "too much data before Hello");
  if (eof) drop(c, "peer closed");
}

void Server::handle_frame(Conn& c, std::string frame) {
  flatbuffers::Verifier::Options vo;
  vo.max_size = opts_.limits.max_frame_bytes;
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), vo);
  if (!R::VerifyMessageBuffer(v)) return drop(c, "frame does not verify as TSG1 Message");
  const auto* m = R::GetMessage(frame.data());
  if (m->payload_type() != R::Payload::Request) return drop(c, "clients may only send requests");
  const auto* req = m->payload_as_Request();
  if (!req || req->value_type() == R::RequestPayload::NONE ||
      static_cast<int>(req->value_type()) > static_cast<int>(R::RequestPayload::MAX))
    return drop(c, "unknown or missing request variant");
  if (!m->request_id() || *m->request_id() == 0) return drop(c, "request_id is required and must be >= 1");
  uint64_t rid = *m->request_id();
  if (c.inflight.count(rid)) return drop(c, "duplicate in-flight request_id");
  auto fail = [&](Tsugumi::Data::FailureKind kind, const char* msg) {
    flatbuffers::FlatBufferBuilder b;
    auto d = b.CreateString(msg);
    auto args = b.CreateVector(std::vector<flatbuffers::Offset<Tsugumi::Data::KeyValue>>{});
    Tsugumi::Data::FailureBuilder fb(b);
    fb.add_kind(kind);
    fb.add_args(args);
    fb.add_diagnostic(d);
    auto f = fb.Finish();
    auto r = R::CreateReply(b, R::ReplyPayload::Failure, f.Union());
    R::MessageBuilder mb(b);
    mb.add_request_id(rid);
    mb.add_payload_type(R::Payload::Reply);
    mb.add_payload(r.Union());
    R::FinishMessageBuffer(b, mb.Finish());
    enqueue_raw(c, std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()), false);
  };
  RequestIn in;
  in.conn = c.id;
  in.request_id = rid;
  in.frame = std::make_shared<const std::string>(std::move(frame));
  in.websocket = c.websocket;
  if (req->value_type() == R::RequestPayload::SessionHelloRequest) {
    if (c.hello_seen) return fail(Tsugumi::Data::FailureKind::invalid_request, "Hello was already sent on this connection");
    c.hello_seen = true;
    auto [bytes, ok] = sink_->hello(in);
    c.authenticated = ok;
    enqueue_raw(c, std::move(bytes), false);
    if (!ok) c.close_after_flush = true;
    return;
  }
  if (!c.authenticated)
    return fail(Tsugumi::Data::FailureKind::unauthenticated, "SessionHello must succeed first");
  if (c.inflight.size() >= opts_.limits.max_inflight)
    return fail(Tsugumi::Data::FailureKind::busy, "too many requests in flight on this connection");
  c.inflight.insert(rid);
  sink_->dispatch(std::move(in));
}

void Server::loop() {
  while (!stopping_) {
    {
      std::vector<Command> cmds;
      {
        std::lock_guard<std::mutex> l(inbox_mu_);
        cmds.swap(inbox_);
      }
      for (auto& c : cmds) apply(c);
    }
    int64_t now = mono_ms();
    std::vector<pollfd> fds;
    std::vector<ConnId> ids;
    fds.push_back({wake_r_, POLLIN, 0});
    ids.push_back(0);
    if (unix_fd_ >= 0) { fds.push_back({unix_fd_, POLLIN, 0}); ids.push_back(0); }
    if (ws_unix_fd_ >= 0) { fds.push_back({ws_unix_fd_, POLLIN, 0}); ids.push_back(0); }
    if (tcp_fd_ >= 0) { fds.push_back({tcp_fd_, POLLIN, 0}); ids.push_back(0); }
    for (auto it = conns_.begin(); it != conns_.end();) {
      Conn& c = *it->second;
      if (!c.authenticated && !c.closing && now - c.connected_at > opts_.limits.hello_timeout_ms)
        drop(c, "no Hello within 5 seconds");
      if (c.close_after_flush && c.outq.empty() && c.wpos >= c.wbuf.size()) c.closing = true;
      if (c.closing && c.wpos >= c.wbuf.size()) {
        ::close(c.fd);
        ConnId id = c.id;
        it = conns_.erase(it);
        conn_count_ = conns_.size();
        sink_->disconnected(id);
        continue;
      }
      short ev = POLLIN;
      if (c.wpos < c.wbuf.size() || !c.outq.empty()) ev |= POLLOUT;
      fds.push_back({c.fd, ev, 0});
      ids.push_back(c.id);
      ++it;
    }
    int rc = ::poll(fds.data(), fds.size(), 500);
    if (rc < 0 && errno != EINTR) break;
    size_t i = 0;
    if (fds[i].revents & POLLIN) {
      char tmp[256];
      while (::read(wake_r_, tmp, sizeof tmp) > 0) {}
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
