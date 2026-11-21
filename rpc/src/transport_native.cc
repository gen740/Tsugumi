#include <curl/curl.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "tsugumi/rpc/client_transport.hh"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/rpc/unix_socket.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::client {
namespace {
namespace frame = rpc::frame;

using Clock = std::chrono::steady_clock;

auto RandomBytes(size_t count) -> Bytes {
  std::random_device random;
  Bytes result(count);
  for (auto& value : result) value = static_cast<uint8_t>(random());
  return result;
}

auto Base64(std::span<const uint8_t> bytes) -> std::string {
  return crypto::base64(bytes.data(), bytes.size());
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
    explicit State(TransportOptions value) : options(std::move(value)) {
      if (::pipe(wake) != 0)
        throw std::runtime_error("cannot create RPC wake pipe");
      for (auto descriptor : wake) {
        ::fcntl(descriptor, F_SETFL, O_NONBLOCK);
        ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
      }
    }
    TransportOptions options;
    TransportEvents events;
    std::atomic<bool> stopping{false};
    std::atomic<bool> opened{false};
    std::mutex mutex;
    std::deque<Bytes> outgoing;
    int fd = -1;
    int wake[2];
    CURL* curl = nullptr;
    bool websocket = false;
    Bytes incoming;
    Bytes writing;
    size_t write_offset = 0;

    ~State() {
      ::close(wake[0]);
      ::close(wake[1]);
      if (curl)
        curl_easy_cleanup(curl);
      else if (fd >= 0)
        ::close(fd);
    }

    auto Wake() -> void {
      const char byte = 0;
      (void)::write(wake[1], &byte, 1);
    }

    auto Wait(short flags, std::optional<Clock::time_point> deadline = {})
        -> short {
      pollfd descriptors[] = {{fd, flags, 0}, {wake[0], POLLIN, 0}};
      const auto remaining =
          deadline ? std::max<int64_t>(
                         0, std::chrono::ceil<std::chrono::milliseconds>(
                                *deadline - Clock::now())
                                .count())
                   : -1;
      const int result = ::poll(descriptors, 2, static_cast<int>(remaining));
      if (result < 0 && errno != EINTR)
        throw std::runtime_error("socket poll failed");
      char bytes[64];
      while (::read(wake[0], bytes, sizeof bytes) > 0) {
      }
      return descriptors[0].revents;
    }

    auto CheckOpen() -> void {
      if (stopping) throw std::runtime_error("connection closed");
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
      const auto address = unix_socket::Address(path);
      fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) throw std::runtime_error("cannot create Unix socket");
      if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
          ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw std::runtime_error("cannot configure Unix socket");
#ifdef SO_NOSIGPIPE
      int enabled = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
      if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                    sizeof address) == 0)
        return;
      if (errno != EINPROGRESS)
        throw std::runtime_error("cannot connect to Unix socket");
      while (true) {
        CheckOpen();
        if (!Wait(POLLOUT)) continue;
        CheckOpen();
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
      const auto& protocol = options.subprotocol;
      if (protocol.find_first_of("\r\n ,") != std::string::npos)
        throw std::runtime_error("invalid WebSocket subprotocol");
      curl = curl_easy_init();
      if (!curl) throw std::runtime_error("cannot create HTTP transport");
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
      curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
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
      size_t offset = 0;
      while (offset < request.size()) {
        CheckOpen();
        offset +=
            Write(reinterpret_cast<const uint8_t*>(request.data()) + offset,
                  request.size() - offset);
        if (offset < request.size()) Wait(POLLOUT);
      }
      std::string response;
      while (response.find("\r\n\r\n") == std::string::npos) {
        CheckOpen();
        std::array<uint8_t, 4096> block{};
        const auto count = Read(block.data(), block.size());
        if (count == 0)
          throw std::runtime_error("WebSocket handshake ended early");
        if (count < 0) {
          Wait(POLLIN);
          continue;
        }
        // A peer cannot grow the HTTP header buffer indefinitely.
        if (response.size() + static_cast<size_t>(count) > 64 * 1024)
          throw std::runtime_error(
              "WebSocket upgrade headers exceed their limit");
        response.append(reinterpret_cast<const char*>(block.data()),
                        static_cast<size_t>(count));
      }
      const auto header_end = response.find("\r\n\r\n") + 4;
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
          if (available < frame::kPrefixBytes) break;
          const auto size = frame::DecodePrefix(data, options.max_frame_bytes);
          if (available - frame::kPrefixBytes < size) break;
          const auto* body = data + frame::kPrefixBytes;
          Bytes message(body, body + size);
          consumed += frame::kPrefixBytes + size;
          if (events.received) events.received(std::move(message));
          continue;
        }
        if (available < 2) break;
        const uint8_t opcode = data[0] & 15;
        uint64_t length = data[1] & 127;
        size_t header = 2;
        if (length == 126) {
          if (available < 4) break;
          length = static_cast<uint64_t>(data[2]) << 8 | data[3];
          header = 4;
        } else if (length == 127) {
          if (available < 10) break;
          length = 0;
          for (size_t i = 2; i < 10; ++i) length = (length << 8) | data[i];
          header = 10;
        }
        if (length > options.max_frame_bytes)
          throw std::runtime_error("WebSocket frame exceeds its limit");
        if (available - header < length) break;
        const std::span payload(data + header, static_cast<size_t>(length));
        consumed += header + static_cast<size_t>(length);
        if (opcode == 8)
          throw std::runtime_error("WebSocket peer closed the connection");
        if (opcode == 2 && events.received)
          events.received(Bytes(payload.begin(), payload.end()));
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
        std::array<uint8_t, 65536> block;
        while (!stopping) {
          Flush();
          ParseMessages();
          for (size_t count = 0; count < 64 && !stopping; ++count) {
            const auto received = Read(block.data(), block.size());
            if (received == 0)
              throw std::runtime_error("RPC peer closed the connection");
            if (received < 0) break;
            incoming.insert(incoming.end(), block.begin(),
                            block.begin() + received);
            ParseMessages();
          }
          if (events.tick) events.tick();
          bool has_output;
          {
            std::lock_guard lock(mutex);
            has_output = !outgoing.empty();
          }
          Wait(static_cast<short>(
                   POLLIN | ((!writing.empty() || has_output) ? POLLOUT : 0)),
               events.deadline ? events.deadline() : std::nullopt);
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
  explicit NativeTransport(TransportOptions options)
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
    try {
      frame::CheckSize(bytes.size(), state_->options.max_frame_bytes);
    } catch (const frame::Error& error) {
      return {ErrorCode::InvalidArgument, error.what()};
    }
    Bytes framed;
    if (state_->websocket)
      framed = WebSocketFrame(bytes);
    else {
      const auto prefix =
          frame::EncodePrefix(static_cast<uint32_t>(bytes.size()));
      framed.assign(prefix.begin(), prefix.end());
      framed.insert(framed.end(), bytes.begin(), bytes.end());
    }
    std::lock_guard lock(state_->mutex);
    state_->outgoing.push_back(std::move(framed));
    state_->Wake();
    return {};
  }
  auto Close() -> void override {
    state_->stopping = true;
    state_->Wake();
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

auto MakeTransport(const TransportOptions& options)
    -> std::shared_ptr<Transport> {
  return std::make_shared<NativeTransport>(options);
}

}  // namespace tsugumi::client
