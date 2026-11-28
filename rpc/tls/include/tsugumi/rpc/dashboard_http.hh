// One bounded HTTP/1.1 exchange on a TLS stream: the worker's dashboard.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "tsugumi/rpc/peer_tls.hh"

namespace tsugumi::dashboard_http {

inline constexpr size_t kRequestLimit = 64 * 1024;
inline constexpr size_t kReplyLimit = 8 * 1024 * 1024;
inline constexpr std::string_view kHtml = "text/html; charset=utf-8";
inline constexpr std::string_view kJson = "application/json; charset=utf-8";
inline constexpr std::string_view kPolicy =
    "default-src 'none'; script-src 'unsafe-inline'; style-src "
    "'unsafe-inline'; connect-src 'self'; "
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

struct Request {
  std::string method, path, body;
  std::map<std::string, std::string> headers;
};

struct Reply {
  unsigned status;
  std::string type, body;
  // Extra header lines, such as "Set-Cookie: ...".
  std::vector<std::string> headers;
};

// Header names are lowercase. Throws on a malformed or oversized request.
auto ReadRequest(peer_tls::Stream& stream, std::string prefix) -> Request;
auto WriteReply(peer_tls::Stream& stream, const Reply& reply) -> void;

}  // namespace tsugumi::dashboard_http
