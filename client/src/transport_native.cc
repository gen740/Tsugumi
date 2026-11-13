#include <curl/curl.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "tsugumi/client/client.hh"

namespace tsugumi::client {
namespace {

using Clock = std::chrono::steady_clock;

auto RandomBytes(size_t count) -> Bytes {
  std::random_device random;
  Bytes result(count);
  for (auto& value : result) value = static_cast<uint8_t>(random());
  return result;
}

auto Base64(std::span<const uint8_t> bytes) -> std::string {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const auto a = bytes[i];
    const auto b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
    const auto c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    result += alphabet[a >> 2];
    result += alphabet[((a & 3) << 4) | (b >> 4)];
    result += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
    result += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
  }
  return result;
}

// SHA-1 is used only for RFC 6455's handshake challenge, never for trust or
// TLS.
auto WebSocketAccept(std::string key) -> std::string {
  key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  Bytes data(key.begin(), key.end());
  const uint64_t bits = data.size() * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i)
    data.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  std::array<uint32_t, 5> hash{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
                               0xc3d2e1f0};
  for (size_t offset = 0; offset < data.size(); offset += 64) {
    std::array<uint32_t, 80> words{};
    for (size_t i = 0; i < 16; ++i)
      for (size_t j = 0; j < 4; ++j)
        words[i] = (words[i] << 8) | data[offset + i * 4 + j];
    for (size_t i = 16; i < 80; ++i)
      words[i] = std::rotl(
          words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    auto [a, b, c, d, e] = hash;
    for (size_t i = 0; i < 80; ++i) {
      const uint32_t f = i < 20   ? (b & c) | (~b & d)
                         : i < 40 ? b ^ c ^ d
                         : i < 60 ? (b & c) | (b & d) | (c & d)
                                  : b ^ c ^ d;
      const uint32_t k = i < 20   ? 0x5a827999
                         : i < 40 ? 0x6ed9eba1
                         : i < 60 ? 0x8f1bbcdc
                                  : 0xca62c1d6;
      const auto next = std::rotl(a, 5) + f + e + k + words[i];
      e = d;
      d = c;
      c = std::rotl(b, 30);
      b = a;
      a = next;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
  }
  Bytes digest;
  for (auto word : hash)
    for (int i = 3; i >= 0; --i)
      digest.push_back(static_cast<uint8_t>(word >> (8 * i)));
  return Base64(digest);
}

auto Lower(std::string value) -> std::string {
  for (auto& byte : value)
    byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
  return value;
}
auto Trim(std::string value) -> std::string {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
auto HasToken(std::string value, std::string_view wanted) -> bool {
  value = Lower(std::move(value));
  size_t start = 0;
  while (start < value.size()) {
    const auto end = value.find(',', start);
    if (Trim(value.substr(start, end - start)) == wanted) return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

auto WebSocketFrame(std::span<const uint8_t> message, uint8_t opcode = 2)
    -> Bytes {
  Bytes frame{static_cast<uint8_t>(0x80 | opcode)};
  if (message.size() < 126)
    frame.push_back(static_cast<uint8_t>(0x80 | message.size()));
  else if (message.size() <= 65535) {
    frame.push_back(0xfe);
    frame.push_back(static_cast<uint8_t>(message.size() >> 8));
    frame.push_back(static_cast<uint8_t>(message.size()));
  } else {
    frame.push_back(0xff);
    const uint64_t length = message.size();
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<uint8_t>(length >> (8 * i)));
  }
  const auto mask = RandomBytes(4);
  frame.insert(frame.end(), mask.begin(), mask.end());
  for (size_t i = 0; i < message.size(); ++i)
    frame.push_back(message[i] ^ mask[i % 4]);
  return frame;
}

class NativeTransport final : public Transport {
  struct State {
    explicit State(Options value) : options(std::move(value)) {}
    Options options;
    TransportEvents events;
    std::atomic<bool> stopping{false};
    std::atomic<bool> opened{false};
    std::mutex mutex;
    std::deque<Bytes> outgoing;
    size_t queued_bytes = 0;
    int fd = -1;
    CURL* curl = nullptr;
    bool websocket = false;
    Bytes incoming;
    Bytes fragments;
    bool fragmenting = false;
    Bytes writing;
    size_t write_offset = 0;

    ~State() {
      if (curl)
        curl_easy_cleanup(curl);
      else if (fd >= 0)
        ::close(fd);
    }

    auto Wait(short flags) -> void {
      pollfd descriptor{fd, flags, 0};
      const int result = ::poll(&descriptor, 1, 20);
      if (result < 0 && errno != EINTR)
        throw std::runtime_error("socket poll failed");
    }

    auto CheckDeadline(Clock::time_point deadline) -> void {
      if (stopping) throw std::runtime_error("connection closed");
      if (Clock::now() >= deadline)
        throw std::runtime_error("connection timed out");
    }

    auto Read(uint8_t* bytes, size_t size) -> ptrdiff_t {
      if (curl) {
        size_t received = 0;
        const auto code = curl_easy_recv(curl, bytes, size, &received);
        if (code == CURLE_AGAIN) return -1;
        if (code != CURLE_OK)
          throw std::runtime_error("WebSocket receive failed");
        return static_cast<ptrdiff_t>(received);
      }
      const auto result = ::recv(fd, bytes, size, 0);
      if (result < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return -1;
      if (result < 0) throw std::runtime_error("Unix socket receive failed");
      return result;
    }

    auto Write(const uint8_t* bytes, size_t size) -> size_t {
      if (curl) {
        size_t sent = 0;
        const auto code = curl_easy_send(curl, bytes, size, &sent);
        if (code == CURLE_AGAIN) return 0;
        if (code != CURLE_OK) throw std::runtime_error("WebSocket send failed");
        return sent;
      }
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto result = ::send(fd, bytes, size, flags);
      if (result < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
      if (result <= 0) throw std::runtime_error("Unix socket send failed");
      return static_cast<size_t>(result);
    }

    auto ConnectUnix() -> void {
      auto path = options.endpoint.substr(5);
      if (path.empty() || path.front() != '/')
        throw std::runtime_error("Unix socket path must be absolute");
      sockaddr_un address{};
      if (path.size() >= sizeof address.sun_path)
        throw std::runtime_error("Unix socket path is too long");
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) throw std::runtime_error("cannot create Unix socket");
      if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
          ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw std::runtime_error("cannot configure Unix socket");
#ifdef SO_NOSIGPIPE
      int enabled = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
      if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                    sizeof address) == 0)
        return;
      if (errno != EINPROGRESS)
        throw std::runtime_error("cannot connect to Unix socket");
      const auto deadline = Clock::now() + options.connect_timeout;
      while (true) {
        CheckDeadline(deadline);
        pollfd descriptor{fd, POLLOUT, 0};
        if (::poll(&descriptor, 1, 20) <= 0) continue;
        int error = 0;
        socklen_t size = sizeof error;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error)
          throw std::runtime_error("cannot connect to Unix socket");
        return;
      }
    }

    auto ConnectWebSocket() -> void {
      static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
      if (initialized != CURLE_OK)
        throw std::runtime_error("cannot initialize HTTP transport");
      std::string url = options.endpoint;
      if (url.starts_with("ws://"))
        url.replace(0, 2, "http");
      else if (url.starts_with("wss://"))
        url.replace(0, 3, "https");
      else
        throw std::runtime_error("unsupported endpoint scheme");
      std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(
          curl_url(), curl_url_cleanup);
      if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(),
                                  0) != CURLUE_OK)
        throw std::runtime_error("invalid WebSocket endpoint");
      const auto part = [&](CURLUPart field) {
        char* value = nullptr;
        if (curl_url_get(parsed.get(), field, &value, 0) != CURLUE_OK)
          return std::string{};
        std::string result(value);
        curl_free(value);
        return result;
      };
      if (!part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty() ||
          !part(CURLUPART_FRAGMENT).empty())
        throw std::runtime_error(
            "WebSocket endpoint must not contain credentials or a fragment");
      auto host = part(CURLUPART_HOST);
      const auto port = part(CURLUPART_PORT);
      if (!port.empty()) host += ':' + port;
      auto target = part(CURLUPART_PATH);
      if (target.empty()) target = "/";
      const auto query = part(CURLUPART_QUERY);
      if (!query.empty()) target += '?' + query;
      const auto protocol =
          options.subprotocol.empty()
              ? options.protocol == Protocol::Agent  ? "tsugumi.agent.v1"
                : options.protocol == Protocol::Host ? "tsugumi.host.v1"
                                                     : "tsugumi.rpc.v1"
              : options.subprotocol;
      if (protocol.find_first_of("\r\n ,") != std::string::npos)
        throw std::runtime_error("invalid WebSocket subprotocol");
      curl = curl_easy_init();
      if (!curl) throw std::runtime_error("cannot create HTTP transport");
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
      curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                       static_cast<long>(options.connect_timeout.count()));
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                       static_cast<long>(options.connect_timeout.count()));
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(
          curl, CURLOPT_XFERINFOFUNCTION,
          +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            return static_cast<State*>(context)->stopping.load() ? 1 : 0;
          });
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
      const auto connected = curl_easy_perform(curl);
      if (connected != CURLE_OK)
        throw std::runtime_error(std::string("WebSocket connection failed: ") +
                                 curl_easy_strerror(connected));
      curl_socket_t socket = CURL_SOCKET_BAD;
      curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket);
      if (socket == CURL_SOCKET_BAD)
        throw std::runtime_error("HTTP transport has no socket");
      fd = socket;
      const auto key = Base64(RandomBytes(16));
      const auto request =
          "GET " + target + " HTTP/1.1\r\nHost: " + host +
          "\r\nUpgrade: websocket\r\nConnection: "
          "Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
          key + "\r\nSec-WebSocket-Protocol: " + protocol + "\r\n\r\n";
      const auto deadline = Clock::now() + options.connect_timeout;
      size_t offset = 0;
      while (offset < request.size()) {
        CheckDeadline(deadline);
        offset +=
            Write(reinterpret_cast<const uint8_t*>(request.data()) + offset,
                  request.size() - offset);
        if (offset < request.size()) Wait(POLLOUT);
      }
      std::string response;
      while (response.find("\r\n\r\n") == std::string::npos) {
        CheckDeadline(deadline);
        std::array<uint8_t, 4096> block{};
        const auto count = Read(block.data(), block.size());
        if (count == 0)
          throw std::runtime_error("WebSocket handshake ended early");
        if (count < 0) {
          Wait(POLLIN);
          continue;
        }
        response.append(reinterpret_cast<const char*>(block.data()),
                        static_cast<size_t>(count));
        if (response.size() > 16384)
          throw std::runtime_error("WebSocket handshake too large");
      }
      const auto header_end = response.find("\r\n\r\n") + 4;
      if (!response.starts_with("HTTP/1.1 101 "))
        throw std::runtime_error("WebSocket upgrade rejected");
      std::map<std::string, std::string> headers;
      size_t position = response.find("\r\n") + 2;
      while (position + 2 < header_end) {
        const auto end = response.find("\r\n", position);
        const auto line = response.substr(position, end - position);
        const auto colon = line.find(':');
        if (colon == std::string::npos)
          throw std::runtime_error("invalid WebSocket response header");
        const auto name = Lower(Trim(line.substr(0, colon)));
        if (!headers.emplace(name, Trim(line.substr(colon + 1))).second)
          throw std::runtime_error("duplicate WebSocket response header");
        position = end + 2;
      }
      if (!HasToken(headers["upgrade"], "websocket") ||
          !HasToken(headers["connection"], "upgrade") ||
          headers["sec-websocket-accept"] != WebSocketAccept(key) ||
          headers["sec-websocket-protocol"] != protocol ||
          !headers["sec-websocket-extensions"].empty())
        throw std::runtime_error("invalid WebSocket handshake response");
      incoming.assign(response.begin() + static_cast<ptrdiff_t>(header_end),
                      response.end());
      websocket = true;
    }

    auto ParseMessages() -> void {
      size_t consumed = 0;
      while (!stopping) {
        const auto available = incoming.size() - consumed;
        const auto* data = incoming.data() + consumed;
        if (!websocket) {
          if (available < 4) break;
          const uint32_t size = static_cast<uint32_t>(data[0]) |
                                static_cast<uint32_t>(data[1]) << 8 |
                                static_cast<uint32_t>(data[2]) << 16 |
                                static_cast<uint32_t>(data[3]) << 24;
          if (!size || size > options.max_frame_bytes)
            throw std::runtime_error("invalid RPC frame length");
          if (available - 4 < size) break;
          Bytes message(data + 4, data + 4 + size);
          consumed += 4 + size;
          if (events.received) events.received(std::move(message));
          continue;
        }
        if (available < 2) break;
        const bool final = data[0] & 0x80;
        const uint8_t opcode = data[0] & 15;
        if ((data[0] & 0x70) || (data[1] & 0x80))
          throw std::runtime_error("invalid WebSocket frame flags");
        uint64_t length = data[1] & 127;
        size_t header = 2;
        if (length == 126) {
          if (available < 4) break;
          length = static_cast<uint64_t>(data[2]) << 8 | data[3];
          header = 4;
          if (length < 126)
            throw std::runtime_error("noncanonical WebSocket frame length");
        } else if (length == 127) {
          if (available < 10) break;
          if (data[2] & 0x80)
            throw std::runtime_error("invalid WebSocket frame length");
          length = 0;
          for (size_t i = 2; i < 10; ++i) length = (length << 8) | data[i];
          header = 10;
          if (length < 65536)
            throw std::runtime_error("noncanonical WebSocket frame length");
        }
        const bool control = opcode >= 8;
        if (length > options.max_frame_bytes ||
            (control && (!final || length > 125)))
          throw std::runtime_error("WebSocket frame exceeds its limit");
        if (available - header < length) break;
        const std::span payload(data + header, static_cast<size_t>(length));
        consumed += header + static_cast<size_t>(length);
        if (opcode == 8)
          throw std::runtime_error("WebSocket peer closed the connection");
        if (opcode == 9) {
          std::lock_guard lock(mutex);
          auto pong = WebSocketFrame(payload, 10);
          if (queued_bytes + pong.size() > options.max_queued_bytes)
            throw std::runtime_error("WebSocket outbox is full");
          queued_bytes += pong.size();
          outgoing.push_back(std::move(pong));
          continue;
        }
        if (opcode == 10) continue;
        if (opcode != 0 && opcode != 2)
          throw std::runtime_error("RPC requires binary WebSocket messages");
        if ((opcode == 0) != fragmenting)
          throw std::runtime_error("invalid WebSocket fragmentation");
        if (fragments.size() + length > options.max_frame_bytes)
          throw std::runtime_error("RPC message exceeds its limit");
        fragments.insert(fragments.end(), payload.begin(), payload.end());
        fragmenting = !final;
        if (final) {
          if (fragments.empty()) throw std::runtime_error("empty RPC message");
          Bytes message;
          message.swap(fragments);
          if (events.received) events.received(std::move(message));
        }
      }
      incoming.erase(incoming.begin(),
                     incoming.begin() + static_cast<ptrdiff_t>(consumed));
    }

    auto Flush() -> void {
      for (size_t count = 0; count < 64 && !stopping; ++count) {
        if (writing.empty()) {
          std::lock_guard lock(mutex);
          if (outgoing.empty()) return;
          writing = std::move(outgoing.front());
          outgoing.pop_front();
          write_offset = 0;
        }
        const auto sent =
            Write(writing.data() + write_offset, writing.size() - write_offset);
        write_offset += sent;
        if (write_offset != writing.size()) return;
        {
          std::lock_guard lock(mutex);
          queued_bytes -= writing.size();
        }
        writing.clear();
      }
    }

    auto Run() -> void {
      Error error{ErrorCode::Closed, "RPC connection closed"};
      try {
        if (options.endpoint.starts_with("unix:"))
          ConnectUnix();
        else
          ConnectWebSocket();
        if (stopping) throw std::runtime_error("connection closed");
        opened = true;
        if (events.opened) events.opened();
        while (!stopping) {
          Flush();
          ParseMessages();
          std::array<uint8_t, 65536> block{};
          for (size_t count = 0; count < 64 && !stopping; ++count) {
            const auto received = Read(block.data(), block.size());
            if (received == 0)
              throw std::runtime_error("RPC peer closed the connection");
            if (received < 0) break;
            incoming.insert(incoming.end(), block.begin(),
                            block.begin() + received);
            ParseMessages();
            if (incoming.size() > options.max_frame_bytes + 14)
              throw std::runtime_error("RPC receive buffer exceeds its limit");
          }
          if (events.tick) events.tick();
          bool has_output;
          {
            std::lock_guard lock(mutex);
            has_output = !outgoing.empty();
          }
          Wait(static_cast<short>(
              POLLIN | ((!writing.empty() || has_output) ? POLLOUT : 0)));
        }
      } catch (const std::exception& failure) {
        if (!stopping) error = {ErrorCode::Transport, failure.what()};
      } catch (...) {
        error = {ErrorCode::Transport, "RPC transport failed"};
      }
      opened = false;
      stopping = true;
      if (curl) {
        curl_easy_cleanup(curl);
        curl = nullptr;
      } else if (fd >= 0)
        ::close(fd);
      fd = -1;
      if (events.closed) events.closed(std::move(error));
    }
  };

 public:
  explicit NativeTransport(Options options)
      : state_(std::make_shared<State>(std::move(options))) {}
  ~NativeTransport() override { Close(); }

  auto Start(TransportEvents events) -> void override {
    std::lock_guard lock(lifecycle_);
    if (thread_.joinable())
      throw std::runtime_error("transport already started");
    state_->events = std::move(events);
    thread_ = std::thread([state = state_] { state->Run(); });
  }
  auto Send(Bytes bytes) -> Error override {
    if (!state_->opened || state_->stopping)
      return {ErrorCode::Closed, "transport is not open"};
    if (bytes.empty() || bytes.size() > state_->options.max_frame_bytes ||
        bytes.size() > UINT32_MAX)
      return {ErrorCode::InvalidArgument, "RPC frame exceeds its limit"};
    Bytes framed;
    if (state_->websocket)
      framed = WebSocketFrame(bytes);
    else {
      const auto size = static_cast<uint32_t>(bytes.size());
      for (unsigned shift = 0; shift < 32; shift += 8)
        framed.push_back(static_cast<uint8_t>(size >> shift));
      framed.insert(framed.end(), bytes.begin(), bytes.end());
    }
    std::lock_guard lock(state_->mutex);
    if (state_->queued_bytes + framed.size() > state_->options.max_queued_bytes)
      return {ErrorCode::Backpressure, "RPC transport outbox is full"};
    state_->queued_bytes += framed.size();
    state_->outgoing.push_back(std::move(framed));
    return {};
  }
  auto Close() -> void override {
    state_->stopping = true;
    std::thread closing;
    {
      std::lock_guard lock(lifecycle_);
      if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id())
          thread_.detach();
        else
          closing = std::move(thread_);
      }
    }
    if (closing.joinable()) closing.join();
  }

 private:
  std::shared_ptr<State> state_;
  std::mutex lifecycle_;
  std::thread thread_;
};

}  // namespace

auto MakeTransport(const Options& options) -> std::shared_ptr<Transport> {
  if (options.max_frame_bytes == 0 || options.max_frame_bytes > UINT32_MAX ||
      options.max_queued_bytes < options.max_frame_bytes ||
      options.connect_timeout.count() <= 0)
    throw std::invalid_argument("invalid RPC transport limits");
  return std::make_shared<NativeTransport>(options);
}

}  // namespace tsugumi::client
