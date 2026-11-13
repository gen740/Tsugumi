// Real TLS handshakes over socketpair: no listener or network permission
// needed.
#include "util/peer_tls.hh"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "agent_fixture_server.hh"

namespace tsugumi::test {
namespace {
struct Result {
  bool client = false, server = false;
  std::string received;
};
auto Exchange(const peer_tls::Context& server, const peer_tls::Context& client)
    -> Result {
  int pair[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
    throw std::runtime_error("socketpair failed");
  ::signal(SIGPIPE, SIG_IGN);
  for (int fd : pair) {
    // Darwin accepts inherit O_NONBLOCK from a nonblocking listener. Recreate
    // that state without bind(), then exercise the product's socket setup.
    if (::fcntl(fd, F_SETFL, O_NONBLOCK))
      throw std::runtime_error("cannot make fixture socket nonblocking");
    peer_tls::ConfigureSocket(fd, 2);
    EXPECT_EQ(::fcntl(fd, F_GETFL) & O_NONBLOCK, 0);
  }
  Result result;
  std::thread peer([&] {
    try {
      peer_tls::Stream stream(server, pair[0], true);
      result.server = true;
      result.received = stream.Read(4);
      stream.Write("PONG");
    } catch (...) {
    }
    ::close(pair[0]);
  });
  try {
    peer_tls::Stream stream(client, pair[1], false);
    for (char byte : std::string("PING")) {
      stream.Write(std::string_view(&byte, 1));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    result.client = stream.Read(4) == "PONG";
  } catch (...) {
  }
  ::close(pair[1]);
  peer.join();
  return result;
}
TEST(PeerTLS, MutualPinsAcceptAndAChangedWorkerKeyGetsNoRequestBytes) {
  TemporaryDirectory directory;
  peer_tls::Identity worker(directory.path / "worker.pem"),
      host(directory.path / "host.pem"), other(directory.path / "other.pem");
  peer_tls::Context server(worker, true, [&](std::string_view key) {
    return key == host.public_key();
  });
  peer_tls::Context client(host, false, [&](std::string_view key) {
    return key == worker.public_key();
  });
  auto accepted = Exchange(server, client);
  EXPECT_TRUE(accepted.client);
  EXPECT_TRUE(accepted.server);
  EXPECT_EQ(accepted.received, "PING");
  peer_tls::Context wrong(host, false, [&](std::string_view key) {
    return key == other.public_key();
  });
  auto refused = Exchange(server, wrong);
  EXPECT_FALSE(refused.client);
  EXPECT_TRUE(refused.received.empty());
}
TEST(PeerTLS, AuthorizedKeysAreRequiredAndRemovalRevokesTheNextHandshake) {
  TemporaryDirectory directory;
  peer_tls::Identity worker(directory.path / "worker.pem"),
      host(directory.path / "host.pem"),
      stranger(directory.path / "stranger.pem");
  const auto file = directory.path / "authorized_keys";
  Write(file, "# Host key\n\n" + host.public_key() + " # workstation\n");
  peer_tls::Context server(worker, true, [&](std::string_view key) {
    return peer_tls::Authorized(file, key);
  });
  peer_tls::Context client(host, false, [&](std::string_view key) {
    return key == worker.public_key();
  });
  EXPECT_TRUE(Exchange(server, client).client);
  Write(file, stranger.public_key() + "\n");
  auto removed = Exchange(server, client);
  EXPECT_FALSE(removed.client);
  EXPECT_FALSE(removed.server);
  EXPECT_TRUE(removed.received.empty());
  Write(file, host.public_key() + "\ninvalid-key\n");
  EXPECT_FALSE(Exchange(server, client).server);
  std::filesystem::remove(file);
  EXPECT_FALSE(Exchange(server, client).server);
}
TEST(PeerTLS, IdentitySurvivesRestartAndRejectsUnsafePrivateFiles) {
  TemporaryDirectory directory;
  const auto file = directory.path / "identity.pem";
  peer_tls::Identity first(file);
  const auto key = first.public_key();
  EXPECT_EQ(peer_tls::Identity(file).public_key(), key);
  EXPECT_NO_THROW(peer_tls::ValidateKey(key));
  EXPECT_THROW(peer_tls::ValidateKey("ed25519:invalid"), std::runtime_error);
  struct stat st{};
  ASSERT_EQ(::stat(file.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
  ::chmod(file.c_str(), 0644);
  EXPECT_THROW(peer_tls::Identity{file}, std::runtime_error);
  ::chmod(file.c_str(), 0600);
  std::filesystem::create_symlink(file, directory.path / "symlink.pem");
  EXPECT_THROW(peer_tls::Identity{directory.path / "symlink.pem"},
               std::runtime_error);
}
TEST(PeerTLS, MissingClientCertificateAndTLS12AreRejected) {
  TemporaryDirectory directory;
  peer_tls::Identity worker(directory.path / "worker.pem");
  peer_tls::Context server(worker, true, [](std::string_view) { return true; });
  for (int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    ::signal(SIGPIPE, SIG_IGN);
    std::atomic<bool> admitted = false;
    std::thread peer([&] {
      try {
        peer_tls::Stream stream(server, pair[0], true);
        admitted = true;
      } catch (...) {
      }
      ::close(pair[0]);
    });
    auto* ctx = SSL_CTX_new(TLS_method());
    SSL_CTX_set_min_proto_version(ctx, version);
    SSL_CTX_set_max_proto_version(ctx, version);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE,
                       nullptr);  // Test-only uncredentialed adversary.
    auto* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, pair[1]);
    if (SSL_connect(ssl) == 1) {
      char c;
      SSL_read(ssl, &c, 1);
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(pair[1]);
    peer.join();
    EXPECT_FALSE(admitted);
  }
}
}  // namespace
}  // namespace tsugumi::test
