#include "tsugumi/rpc/dashboard_http.hh"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace tsugumi::dashboard_http {
namespace {
constexpr size_t kHeadLimit = 8 * 1024;

auto Require(bool ok, const char* error) -> void {
  if (!ok) throw std::runtime_error(error);
}

struct Message {
  std::string line, body;
  std::map<std::string, std::string> headers;
};

auto Number(std::string_view value) -> size_t {
  size_t number = 0;
  const auto* end = value.data() + value.size();
  const auto [rest, error] = std::from_chars(value.data(), end, number);
  Require(error == std::errc{} && rest == end, "Invalid HTTP number");
  return number;
}

auto Read(peer_tls::Stream& stream, std::string buffer, size_t body_limit)
    -> Message {
  auto end = buffer.find("\r\n\r\n");
  while (end == buffer.npos) {
    Require(buffer.size() < kHeadLimit, "HTTP head too large");
    auto bytes = stream.ReadSome(kHeadLimit - buffer.size());
    Require(!bytes.empty(), "Incomplete HTTP head");
    buffer += bytes;
    end = buffer.find("\r\n\r\n");
  }
  auto head = std::string_view(buffer).substr(0, end);
  auto line = head.substr(0, head.find("\r\n"));
  Message message;
  message.line = line;
  head.remove_prefix(line.size());
  while (!head.empty()) {
    head.remove_prefix(2);
    line = head.substr(0, head.find("\r\n"));
    head.remove_prefix(line.size());
    const auto colon = line.find(':');
    Require(colon != line.npos && colon > 0, "Invalid HTTP header");
    std::string name(line.substr(0, colon));
    for (auto& c : name) {
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      Require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-',
              "Invalid HTTP header name");
    }
    auto value = line.substr(colon + 1);
    const auto first = value.find_first_not_of(" \t");
    value =
        first == value.npos
            ? std::string_view{}
            : value.substr(first, value.find_last_not_of(" \t") - first + 1);
    Require(std::ranges::all_of(
                value, [](unsigned char c) { return c >= 32 && c < 127; }),
            "Invalid HTTP header value");
    Require(message.headers.emplace(std::move(name), value).second,
            "Duplicate HTTP header");
  }
  Require(!message.headers.contains("transfer-encoding"),
          "Unsupported HTTP transfer encoding");
  const auto length = message.headers.find("content-length");
  const auto size =
      length == message.headers.end() ? 0 : Number(length->second);
  message.body = buffer.substr(end + 4);
  Require(size <= body_limit && message.body.size() <= size,
          "Invalid HTTP body length");
  message.body += stream.Read(size - message.body.size());
  return message;
}
}  // namespace

auto ReadRequest(peer_tls::Stream& stream, std::string prefix) -> Request {
  auto message = Read(stream, std::move(prefix), kRequestLimit);
  auto line = std::string_view(message.line);
  constexpr std::string_view version = " HTTP/1.1";
  Require(line.ends_with(version), "Expected HTTP/1.1");
  line.remove_suffix(version.size());
  const auto split = line.find(' ');
  Require(split != line.npos, "Invalid HTTP request");
  return {std::string(line.substr(0, split)),
          std::string(line.substr(split + 1)), std::move(message.body),
          std::move(message.headers)};
}

auto WriteReply(peer_tls::Stream& stream, const Reply& reply) -> void {
  Require(reply.body.size() <= kReplyLimit, "Dashboard reply is too large");
  const auto reason = reply.status == 200   ? "OK"
                      : reply.status == 400 ? "Bad Request"
                      : reply.status == 401 ? "Unauthorized"
                      : reply.status == 403 ? "Forbidden"
                      : reply.status == 404 ? "Not Found"
                      : reply.status == 409 ? "Conflict"
                      : reply.status == 429 ? "Too Many Requests"
                                            : "Internal Server Error";
  std::string extra;
  for (const auto& header : reply.headers) {
    Require(header.find_first_of("\r\n") == header.npos,
            "Invalid HTTP reply header");
    extra += header + "\r\n";
  }
  stream.Write(
      "HTTP/1.1 " + std::to_string(reply.status) + " " + reason +
      "\r\nContent-Type: " + reply.type +
      "\r\nContent-Length: " + std::to_string(reply.body.size()) +
      "\r\nCache-Control: no-store\r\nConnection: close\r\n"
      "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
      "X-Frame-Options: DENY\r\n" +
      extra + "Content-Security-Policy: " + std::string(kPolicy) +
      "\r\n\r\n");
  if (!reply.body.empty()) stream.Write(reply.body);
}

}  // namespace tsugumi::dashboard_http
