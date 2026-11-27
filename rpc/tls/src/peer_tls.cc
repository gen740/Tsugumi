#include "tsugumi/rpc/peer_tls.hh"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "tsugumi/util/fs.hh"

namespace tsugumi::peer_tls {
namespace {
template <class T, auto Free>
using Owned = std::unique_ptr<T, decltype(Free)>;
using Key = Owned<EVP_PKEY, EVP_PKEY_free>;
using Certificate = Owned<X509, X509_free>;
using Bio = Owned<BIO, BIO_free>;
auto Check(bool ok, const char* message) -> void {
  if (!ok) {
    ERR_clear_error();
    throw std::runtime_error(message);
  }
}
auto Public(EVP_PKEY* key) -> std::string {
  Check(key && EVP_PKEY_is_a(key, "ED25519"), "Expected an Ed25519 public key");
  const auto size = i2d_PUBKEY(key, nullptr);
  Check(size > 0 && size < 1024, "Cannot encode public key");
  std::vector<unsigned char> der(static_cast<size_t>(size));
  auto* p = der.data();
  Check(i2d_PUBKEY(key, &p) == size, "Cannot encode public key");
  std::string base64(4 * ((der.size() + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64.data()), der.data(),
                  size);
  return "ed25519:" + base64;
}
auto ReadPrivate(const std::filesystem::path& path) -> std::string {
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0 && errno == ENOENT) return {};
  struct Close {
    int fd;
    ~Close() {
      if (fd >= 0) ::close(fd);
    }
  } close{fd};
  struct stat st{};
  Check(fd >= 0 && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_uid == ::geteuid() && !(st.st_mode & 0077) &&
            st.st_size > 0 && st.st_size <= 16384,
        "TLS identity must be a private owned regular file");
  std::string pem(static_cast<size_t>(st.st_size), '\0');
  size_t offset = 0;
  while (offset < pem.size()) {
    const auto n = ::read(fd, pem.data() + offset, pem.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    Check(n > 0, "Cannot read TLS identity");
    offset += static_cast<size_t>(n);
  }
  return pem;
}
}  // namespace

auto ConfigureSocket(int fd, int timeout_seconds) -> void {
  const int flags = ::fcntl(fd, F_GETFL);
  Check(flags >= 0 && ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0 &&
            ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0,
        "Cannot configure blocking peer socket");
  const int enabled = 1;
  sockaddr_storage address{};
  socklen_t address_size = sizeof address;
  Check(::getsockname(fd, reinterpret_cast<sockaddr*>(&address),
                      &address_size) == 0,
        "Cannot identify peer socket address family");
  if (address.ss_family == AF_INET || address.ss_family == AF_INET6)
    Check(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled) ==
              0,
          "Cannot disable TCP send coalescing");
#ifdef SO_NOSIGPIPE
  Check(
      ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) == 0,
      "Cannot disable socket SIGPIPE");
#endif
  if (timeout_seconds == 0) return;
  timeval timeout{timeout_seconds, 0};
  Check(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ==
                0 &&
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof timeout) == 0,
        "Cannot configure peer socket deadline");
}

auto ValidateKey(std::string_view value) -> void {
  Check(value.starts_with("ed25519:") && value.size() == 68,
        "Enter an ed25519: public key containing base64 SPKI");
  const auto text = value.substr(8);
  std::vector<unsigned char> der(3 * text.size() / 4);
  const int n = EVP_DecodeBlock(
      der.data(), reinterpret_cast<const unsigned char*>(text.data()),
      static_cast<int>(text.size()));
  Check(n >= 0 && text.ends_with("="), "Invalid public key encoding");
  const auto size = n - (text.ends_with("==") ? 2 : 1);
  const auto* p = der.data();
  Key key(d2i_PUBKEY(nullptr, &p, size), EVP_PKEY_free);
  Check(key && p == der.data() + size && Public(key.get()) == value,
        "Invalid or noncanonical Ed25519 SPKI");
}
struct Identity::State {
  Key key{nullptr, EVP_PKEY_free};
  Certificate certificate{nullptr, X509_free};
};
Identity::Identity(const std::filesystem::path& file)
    : state_(std::make_unique<State>()) {
  struct stat parent{};
  Check(::lstat(file.parent_path().c_str(), &parent) == 0 &&
            S_ISDIR(parent.st_mode) && parent.st_uid == ::geteuid() &&
            !(parent.st_mode & 0077),
        "TLS state directory must be private and owned");
  auto pem = ReadPrivate(file);
  if (pem.empty()) {
    state_->key.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"));
    Check(bool(state_->key), "Cannot generate Ed25519 identity");
    state_->certificate.reset(X509_new());
    auto* cert = state_->certificate.get();
    Check(cert && X509_set_version(cert, 2) &&
              ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) &&
              X509_gmtime_adj(X509_getm_notBefore(cert), -300) &&
              X509_gmtime_adj(X509_getm_notAfter(cert), 10L * 365 * 86400) &&
              X509_set_pubkey(cert, state_->key.get()),
          "Cannot create TLS certificate");
    auto* subject = X509_get_subject_name(cert);
    Check(X509_NAME_add_entry_by_txt(
              subject, "CN", MBSTRING_ASC,
              reinterpret_cast<const unsigned char*>("Tsugumi public-key peer"),
              -1, -1, 0) &&
              X509_set_issuer_name(cert, subject) &&
              X509_sign(cert, state_->key.get(), nullptr) > 0,
          "Cannot sign TLS certificate");
    Bio output(BIO_new(BIO_s_mem()), BIO_free);
    Check(output &&
              PEM_write_bio_PrivateKey(output.get(), state_->key.get(), nullptr,
                                       nullptr, 0, nullptr, nullptr) &&
              PEM_write_bio_X509(output.get(), cert),
          "Cannot encode TLS identity");
    char* bytes = nullptr;
    const auto count = BIO_get_mem_data(output.get(), &bytes);
    fs::write_atomic(file, std::string_view(bytes, static_cast<size_t>(count)),
                     0600);
    pem = ReadPrivate(file);
  }
  Bio input(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
            BIO_free);
  state_->key.reset(PEM_read_bio_PrivateKey(
      input.get(), nullptr, [](char*, int, int, void*) { return 0; }, nullptr));
  state_->certificate.reset(
      PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr));
  Check(
      state_->key && state_->certificate &&
          EVP_PKEY_is_a(state_->key.get(), "ED25519") &&
          X509_check_private_key(state_->certificate.get(), state_->key.get()),
      "Invalid TLS identity");
}
Identity::Identity(const std::filesystem::path& file,
                   const std::vector<std::string>& names)
    : Identity(file) {
  Check(!names.empty(), "A server certificate needs at least one name");
  std::string alternatives;
  for (const auto& name : names) {
    Check(!name.empty() && name.find_first_of(", \t\r\n") == name.npos,
          "Invalid server certificate name");
    // An address in brackets is an IPv6 literal, as in URLs.
    auto bare = name.starts_with('[') && name.ends_with(']')
                    ? name.substr(1, name.size() - 2)
                    : name;
    Owned<ASN1_OCTET_STRING, ASN1_OCTET_STRING_free> address(
        a2i_IPADDRESS(bare.c_str()), ASN1_OCTET_STRING_free);
    alternatives += (alternatives.empty() ? "" : ",") +
                    std::string(address ? "IP:" : "DNS:") + bare;
  }
  Certificate cert(X509_new(), X509_free);
  // Fixed serial and validity: Ed25519 signatures are deterministic, so the
  // same key and names give the same certificate. 9999-12-31 is RFC 5280's
  // "no well-defined expiration".
  Owned<ASN1_TIME, ASN1_TIME_free> begin(ASN1_TIME_new(), ASN1_TIME_free);
  Owned<ASN1_TIME, ASN1_TIME_free> end(ASN1_TIME_new(), ASN1_TIME_free);
  Check(cert && begin && end &&
            ASN1_TIME_set_string(begin.get(), "20000101000000Z") &&
            ASN1_TIME_set_string(end.get(), "99991231235959Z") &&
            X509_set_version(cert.get(), 2) &&
            ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) &&
            X509_set1_notBefore(cert.get(), begin.get()) &&
            X509_set1_notAfter(cert.get(), end.get()) &&
            X509_set_pubkey(cert.get(), state_->key.get()),
        "Cannot create TLS certificate");
  auto* subject = X509_get_subject_name(cert.get());
  Check(X509_NAME_add_entry_by_txt(
            subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(names.front().c_str()), -1,
            -1, 0) &&
            X509_set_issuer_name(cert.get(), subject),
        "Cannot create TLS certificate");
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, cert.get(), cert.get(), nullptr, nullptr, 0);
  for (const auto& [nid, value] :
       {std::pair{NID_subject_alt_name, alternatives},
        std::pair{NID_basic_constraints, std::string("critical,CA:FALSE")}}) {
    Owned<X509_EXTENSION, X509_EXTENSION_free> extension(
        X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str()),
        X509_EXTENSION_free);
    Check(extension && X509_add_ext(cert.get(), extension.get(), -1),
          "Cannot create TLS certificate");
  }
  Check(X509_sign(cert.get(), state_->key.get(), nullptr) > 0,
        "Cannot sign TLS certificate");
  state_->certificate = std::move(cert);
}
Identity::~Identity() = default;
auto Identity::public_key() const -> std::string {
  return Public(state_->key.get());
}
auto Identity::certificate_pem() const -> std::string {
  Bio output(BIO_new(BIO_s_mem()), BIO_free);
  Check(output && PEM_write_bio_X509(output.get(), state_->certificate.get()),
        "Cannot encode TLS certificate");
  char* bytes = nullptr;
  const auto count = BIO_get_mem_data(output.get(), &bytes);
  return std::string(bytes, static_cast<size_t>(count));
}
struct Context::State {
  Owned<SSL_CTX, SSL_CTX_free> context{nullptr, SSL_CTX_free};
  std::function<bool(std::string_view)> accepts;
  static auto Verify(X509_STORE_CTX* store, void* opaque) -> int {
    try {
      auto* self = static_cast<State*>(opaque);
      auto* cert = X509_STORE_CTX_get0_cert(store);
      Key key(cert ? X509_get_pubkey(cert) : nullptr, EVP_PKEY_free);
      if (key && self->accepts(Public(key.get()))) {
        X509_STORE_CTX_set_error(store, X509_V_OK);
        return 1;
      }
    } catch (...) {
    }
    X509_STORE_CTX_set_error(store, X509_V_ERR_APPLICATION_VERIFICATION);
    return 0;
  }
};
Context::Context(const Identity& identity, bool server,
                 std::function<bool(std::string_view)> accepts)
    : state_(std::make_unique<State>()) {
  state_->accepts = std::move(accepts);
  state_->context.reset(SSL_CTX_new(TLS_method()));
  auto* ctx = state_->context.get();
  Check(ctx && SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) &&
            SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) &&
            SSL_CTX_use_PrivateKey(ctx, identity.state_->key.get()) &&
            SSL_CTX_use_certificate(ctx, identity.state_->certificate.get()),
        "Cannot configure TLS 1.3 peer");
  SSL_CTX_set_verify(
      ctx, SSL_VERIFY_PEER | (server ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0),
      nullptr);
  SSL_CTX_set_cert_verify_callback(ctx, State::Verify, state_.get());
  SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
  SSL_CTX_set_num_tickets(ctx, 0);
  SSL_CTX_set_max_early_data(ctx, 0);
}
Context::~Context() = default;
struct Stream::State {
  Owned<SSL, SSL_free> ssl{nullptr, SSL_free};
  int fd = -1, downstream = -1;
  std::optional<std::chrono::steady_clock::time_point> deadline;

  auto Remaining() const -> int {
    if (!deadline) return -1;
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(
        *deadline - std::chrono::steady_clock::now());
    Check(left.count() > 0, "TLS stream timed out");
    return static_cast<int>(
        std::min<decltype(left.count())>(left.count(), INT_MAX));
  }

  auto Ready(int rc, const char* failure) -> bool {
    if (rc == 1) return true;
    const int error = SSL_get_error(ssl.get(), rc);
    Check((downstream >= 0 || deadline) &&
              (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE),
          failure);
    const short interest = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
    pollfd sockets[]{{fd, interest, 0}, {downstream, POLLIN, 0}};
    int ready;
    do {
      ready = ::poll(sockets, 2, Remaining());
    } while (ready < 0 && errno == EINTR);
    // Broker clients send one framed request and keep the write side open
    // until the reply. Any further data or EOF ends the exchange.
    Check(ready != 0, "TLS stream timed out");
    Check(ready > 0 && !sockets[1].revents, "TLS downstream disconnected");
    return false;
  }
};
Stream::Stream(const Context& context, int fd, bool server, int downstream,
               std::optional<std::chrono::steady_clock::time_point> deadline)
    : state_(std::make_unique<State>()) {
  state_->fd = fd;
  state_->downstream = downstream;
  state_->deadline = deadline;
  state_->ssl.reset(SSL_new(context.state_->context.get()));
  auto* ssl = state_->ssl.get();
  Check(ssl && SSL_set_fd(ssl, fd), "Cannot open TLS stream");
  constexpr auto failure = "TLS public-key authentication failed";
  for (;;) {
    (void)state_->Remaining();
    if (state_->Ready(server ? SSL_accept(ssl) : SSL_connect(ssl), failure))
      break;
  }
  Check(SSL_get_verify_result(ssl) == X509_V_OK, failure);
}
Stream::~Stream() = default;
auto Stream::Read(size_t count) -> std::string {
  std::string result(count, '\0');
  size_t offset = 0;
  while (offset < count) {
    (void)state_->Remaining();
    size_t n = 0;
    const int rc = SSL_read_ex(state_->ssl.get(), result.data() + offset,
                               count - offset, &n);
    if (!state_->Ready(rc, "TLS stream disconnected or timed out")) continue;
    offset += n;
  }
  return result;
}
auto Stream::ReadSome(size_t limit) -> std::string {
  std::string result(limit, '\0');
  while (true) {
    (void)state_->Remaining();
    size_t n = 0;
    const int rc =
        SSL_read_ex(state_->ssl.get(), result.data(), result.size(), &n);
    if (rc != 1 &&
        SSL_get_error(state_->ssl.get(), rc) == SSL_ERROR_ZERO_RETURN)
      return {};
    if (!state_->Ready(rc, "TLS stream disconnected or timed out")) continue;
    result.resize(n);
    return result;
  }
}
auto Stream::peer_key() const -> std::string {
  Key key(X509_get_pubkey(SSL_get0_peer_certificate(state_->ssl.get())),
          EVP_PKEY_free);
  return Public(key.get());
}
auto Stream::Write(std::string_view bytes) -> void {
  while (!bytes.empty()) {
    (void)state_->Remaining();
    size_t n = 0;
    const int rc =
        SSL_write_ex(state_->ssl.get(), bytes.data(), bytes.size(), &n);
    if (!state_->Ready(rc, "TLS stream disconnected or timed out")) continue;
    bytes.remove_prefix(n);
  }
}
}  // namespace tsugumi::peer_tls
