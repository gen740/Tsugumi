#include "tsugumi/cache/http.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>

namespace tsugumi::cache {
namespace {

constexpr size_t kHeadLimit = 16 * 1024;
constexpr size_t kRead = 64 * 1024;

class Disconnected : public std::runtime_error {
 public:
  Disconnected() : std::runtime_error("The client disconnected") {}
};

auto Lower(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}
auto Trim(std::string_view text) -> std::string_view {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}
auto ParseSize(std::string_view text, int base = 10)
    -> std::optional<uint64_t> {
  uint64_t value = 0;
  const auto* end = text.data() + text.size();
  const auto [rest, error] = std::from_chars(text.data(), end, value, base);
  if (text.empty() || error != std::errc() || rest != end) return std::nullopt;
  return value;
}

class Reader {
 public:
  explicit Reader(Connection& connection) : connection_(connection) {}

  // The request line and headers, or nothing when the peer closed between
  // requests.
  auto Head() -> std::optional<std::string> {
    while (true) {
      if (const auto end = buffer_.find("\r\n\r\n"); end != buffer_.npos) {
        auto head = buffer_.substr(0, end);
        buffer_.erase(0, end + 4);
        return head;
      }
      if (buffer_.size() > kHeadLimit)
        throw std::runtime_error("Request head too large");
      if (!Fill()) {
        if (buffer_.empty()) return std::nullopt;
        throw Disconnected();
      }
    }
  }
  auto Line() -> std::string {
    while (true) {
      if (const auto end = buffer_.find("\r\n"); end != buffer_.npos) {
        auto line = buffer_.substr(0, end);
        buffer_.erase(0, end + 2);
        return line;
      }
      if (buffer_.size() > kHeadLimit)
        throw std::runtime_error("Chunk header too large");
      if (!Fill()) throw Disconnected();
    }
  }
  template <class Consume>
  auto Body(uint64_t size, Consume&& consume) -> void {
    while (size > 0) {
      if (buffer_.empty() && !Fill()) throw Disconnected();
      const auto n =
          static_cast<size_t>(std::min<uint64_t>(size, buffer_.size()));
      consume(std::string_view(buffer_).substr(0, n));
      buffer_.erase(0, n);
      size -= n;
    }
  }

 private:
  auto Fill() -> bool {
    auto bytes = connection_.ReadSome(kRead);
    if (bytes.empty()) return false;
    buffer_ += bytes;
    return true;
  }
  Connection& connection_;
  std::string buffer_;
};

struct Request {
  std::string method, target, version;
  std::map<std::string, std::string> headers;
  auto Header(const std::string& name) const -> std::string {
    const auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
  }
};

auto ParseHead(const std::string& head) -> std::optional<Request> {
  Request request;
  size_t start = 0;
  bool first = true;
  while (start <= head.size()) {
    auto end = head.find("\r\n", start);
    if (end == head.npos) end = head.size();
    const std::string_view line(head.data() + start, end - start);
    start = end + 2;
    if (first) {
      first = false;
      const auto a = line.find(' ');
      const auto b = line.rfind(' ');
      if (a == line.npos || a == b) return std::nullopt;
      request.method = line.substr(0, a);
      request.target = line.substr(a + 1, b - a - 1);
      request.version = line.substr(b + 1);
      if (!request.version.starts_with("HTTP/1.")) return std::nullopt;
      continue;
    }
    const auto colon = line.find(':');
    if (colon == line.npos || colon == 0) return std::nullopt;
    auto name = Lower(std::string(line.substr(0, colon)));
    auto value = std::string(Trim(line.substr(colon + 1)));
    // A repeated framing header could disagree with itself; refuse it.
    if ((name == "content-length" || name == "transfer-encoding") &&
        request.headers.contains(name))
      return std::nullopt;
    request.headers[name] = std::move(value);
  }
  return request;
}

auto Reason(int status) -> std::string_view {
  switch (status) {
    case 100:
      return "Continue";
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 409:
      return "Conflict";
    case 411:
      return "Length Required";
    case 413:
      return "Content Too Large";
    case 415:
      return "Unsupported Media Type";
    case 501:
      return "Not Implemented";
  }
  return "Internal Server Error";
}

// A HEAD response has the headers of the GET response and no body.
auto Respond(Connection& connection, int status, std::string_view body,
             bool close, bool head_only = false) -> void {
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " +
                     std::string(Reason(status)) +
                     "\r\nContent-Type: text/plain; charset=utf-8"
                     "\r\nContent-Length: " +
                     std::to_string(body.size() + 1) + "\r\n";
  if (close) head += "Connection: close\r\n";
  connection.Write(head + "\r\n" +
                   (head_only ? std::string() : std::string(body) + "\n"));
}

auto Send(Connection& connection, const Cache::File& file, bool head_only)
    -> void {
  const int fd = ::open(file.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    Respond(connection, 404, "Not found", false, head_only);
    return;
  }
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } close{fd};
  struct stat st{};
  if (::fstat(fd, &st) != 0) throw std::runtime_error("Cannot read a file");
  auto size = static_cast<uint64_t>(st.st_size);
  connection.Write("HTTP/1.1 200 OK\r\nContent-Type: " + file.content_type +
                   "\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n");
  if (head_only) return;
  // Files are only ever added under their final name, so the size cannot
  // change; a shorter read means the disk failed, and the connection ends.
  std::array<char, kRead> buffer;
  while (size > 0) {
    const auto n =
        ::read(fd, buffer.data(),
               static_cast<size_t>(std::min<uint64_t>(size, buffer.size())));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("Cannot read a cache file");
    connection.Write(std::string_view(buffer.data(), static_cast<size_t>(n)));
    size -= static_cast<uint64_t>(n);
  }
}

// Every Ed25519 SPKI starts with the same bytes; its end tells keys apart.
auto Short(std::string_view key) -> std::string {
  return key.size() <= 20 ? std::string(key)
                          : "..." + std::string(key.substr(key.size() - 13));
}

}  // namespace

auto Serve(Cache& cache, Connection& connection, std::string_view peer)
    -> void {
  Reader reader(connection);
  while (true) {
    const auto head = reader.Head();
    if (!head) return;
    const auto request = ParseHead(*head);
    if (!request) {
      Respond(connection, 400, "Malformed request", true);
      return;
    }
    const bool head_only = request->method == "HEAD";
    const auto connection_header = Lower(request->Header("connection"));
    const bool close = request->version == "HTTP/1.0"
                           ? connection_header != "keep-alive"
                           : connection_header == "close";
    const auto transfer = Lower(request->Header("transfer-encoding"));
    const auto length_header = request->Header("content-length");
    const auto length = ParseSize(length_header);
    if ((!transfer.empty() && transfer != "chunked") ||
        (!transfer.empty() && !length_header.empty()) ||
        (!length_header.empty() && !length)) {
      Respond(connection, 400, "Unsupported request framing", true, head_only);
      return;
    }
    const bool has_body = transfer == "chunked" || (length && *length > 0);
    auto name = std::string_view(request->target);
    name = name.substr(0, name.find('?'));
    if (!name.starts_with('/')) {
      Respond(connection, 400, "Malformed request target", true, head_only);
      return;
    }
    name.remove_prefix(1);

    if (request->method == "GET" || request->method == "HEAD") {
      if (has_body) {
        Respond(connection, 400, "A read carries no body", true, head_only);
        return;
      }
      if (const auto file = cache.Find(name))
        Send(connection, *file, head_only);
      else
        Respond(connection, 404, "Not found", false, head_only);
    } else if (request->method == "PUT") {
      if (transfer.empty() && !length) {
        Respond(connection, 411, "Content-Length required", true);
        return;
      }
      std::unique_ptr<Cache::Upload> upload;
      try {
        upload = cache.Begin(name, Lower(request->Header("content-encoding")));
      } catch (const Rejected& rejected) {
        // The body was not read: the connection cannot carry another request.
        std::cerr << "tsugumi-cached: refused PUT /" << name << " from "
                  << Short(peer) << ": " << rejected.what() << '\n';
        Respond(connection, rejected.status, rejected.what(), true);
        return;
      }
      if (Lower(request->Header("expect")) == "100-continue")
        connection.Write("HTTP/1.1 100 Continue\r\n\r\n");
      // A body cut short throws, and the upload is dropped unserved.
      std::optional<Rejected> refused;
      auto consume = [&](std::string_view bytes) {
        if (refused) return;
        try {
          upload->Write(bytes);
        } catch (const Rejected& rejected) {
          refused = rejected;
        }
      };
      if (transfer == "chunked") {
        while (true) {
          const auto line = reader.Line();
          const auto size = ParseSize(Trim(line.substr(0, line.find(';'))), 16);
          if (!size) {
            Respond(connection, 400, "Malformed chunk", true);
            return;
          }
          if (*size == 0) {
            while (!reader.Line().empty()) {
            }
            break;
          }
          reader.Body(*size, consume);
          if (!reader.Line().empty()) {
            Respond(connection, 400, "Malformed chunk", true);
            return;
          }
        }
      } else {
        reader.Body(*length, consume);
      }
      try {
        if (refused) throw *refused;
        upload->Commit();
      } catch (const Rejected& rejected) {
        std::cerr << "tsugumi-cached: refused PUT /" << name << " from "
                  << Short(peer) << ": " << rejected.what() << '\n';
        Respond(connection, rejected.status, rejected.what(), close);
        if (close) return;
        continue;
      }
      std::cerr << "tsugumi-cached: stored /" << name << " from " << Short(peer)
                << '\n';
      Respond(connection, 200, "Stored", close);
    } else {
      Respond(connection, has_body ? 405 : 501, "Unsupported method", true);
      return;
    }
    if (close) return;
  }
}

}  // namespace tsugumi::cache
