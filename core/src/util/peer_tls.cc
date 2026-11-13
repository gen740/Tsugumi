#include "util/peer_tls.hh"

#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "util/fs.hh"

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
  timeval timeout{timeout_seconds, 0};
  Check(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ==
                0 &&
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof timeout) == 0,
        "Cannot configure peer socket deadline");
#ifdef SO_NOSIGPIPE
  const int enabled = 1;
  Check(
      ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) == 0,
      "Cannot disable socket SIGPIPE");
#endif
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
auto Authorized(const std::filesystem::path& file, std::string_view key)
    -> bool {
  // Re-read at every handshake so removing a line revokes new connections.
  const int fd =
      ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  struct Close {
    int fd;
    ~Close() {
      if (fd >= 0) ::close(fd);
    }
  } close{fd};
  struct stat st{};
  if (fd < 0 || ::fstat(fd, &st) || !S_ISREG(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & 0022) || st.st_size > (1 << 20))
    return false;
  std::string data;
  char buffer[4096];
  while (true) {
    const auto n = ::read(fd, buffer, sizeof buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return false;
    if (!n) break;
    data.append(buffer, static_cast<size_t>(n));
    if (data.size() > (1 << 20)) return false;
  }
  std::istringstream lines(data);
  std::string line;
  bool found = false;
  while (std::getline(lines, line)) {
    line = line.substr(0, line.find('#'));
    std::istringstream fields(line);
    std::string public_key, extra;
    if (!(fields >> public_key)) continue;
    // A malformed configuration fails closed, including unrelated invalid keys.
    if (fields >> extra) return false;
    try {
      ValidateKey(public_key);
    } catch (...) {
      return false;
    }
    found = found || public_key == key;
  }
  return found;
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
Identity::~Identity() = default;
auto Identity::public_key() const -> std::string {
  return Public(state_->key.get());
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
};
Stream::Stream(const Context& context, int fd, bool server)
    : state_(std::make_unique<State>()) {
  state_->ssl.reset(SSL_new(context.state_->context.get()));
  auto* ssl = state_->ssl.get();
  Check(ssl && SSL_set_fd(ssl, fd), "Cannot open TLS stream");
  Check((server ? SSL_accept(ssl) : SSL_connect(ssl)) == 1 &&
            SSL_get_verify_result(ssl) == X509_V_OK,
        "TLS public-key authentication failed");
}
Stream::~Stream() = default;
auto Stream::Read(size_t count) -> std::string {
  std::string result(count, '\0');
  size_t offset = 0;
  while (offset < count) {
    size_t n = 0;
    const int rc = SSL_read_ex(state_->ssl.get(), result.data() + offset,
                               count - offset, &n);
    Check(rc == 1 && n, "TLS stream disconnected or timed out");
    offset += n;
  }
  return result;
}
auto Stream::Write(std::string_view bytes) -> void {
  while (!bytes.empty()) {
    size_t n = 0;
    const int rc =
        SSL_write_ex(state_->ssl.get(), bytes.data(), bytes.size(), &n);
    Check(rc == 1 && n, "TLS stream disconnected or timed out");
    bytes.remove_prefix(n);
  }
}
}  // namespace tsugumi::peer_tls
