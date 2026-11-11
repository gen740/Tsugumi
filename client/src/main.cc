#include <flatbuffers/flexbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection_generated.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include "tsugumi/client/client.hh"

namespace C = tsugumi::client;

namespace {
// Reorder object members without decoding value tokens. In particular, request
// IDs must never pass through a floating-point or signed-only JSON DOM. The
// schema parser below still validates the complete JSON syntax and field types.
class JsonOrder {
 public:
  explicit JsonOrder(std::string_view input) : input_(input) {}

  auto Run() -> std::string {
    auto output = Value(0);
    Whitespace();
    if (position_ != input_.size()) Fail("unexpected trailing input");
    return output;
  }

 private:
  [[noreturn]] static auto Fail(std::string_view message) -> void {
    throw std::runtime_error("invalid request JSON: " + std::string(message));
  }

  auto Whitespace() -> void {
    while (position_ < input_.size() &&
           std::string_view(" \t\r\n").find(input_[position_]) !=
               std::string_view::npos)
      ++position_;
  }

  auto Take(char expected) -> bool {
    Whitespace();
    if (position_ == input_.size() || input_[position_] != expected)
      return false;
    ++position_;
    return true;
  }

  auto String() -> std::string_view {
    Whitespace();
    const auto start = position_;
    if (!Take('"')) Fail("expected a quoted string");
    while (position_ < input_.size()) {
      const auto byte = static_cast<unsigned char>(input_[position_++]);
      if (byte < 32) Fail("control byte in string");
      if (byte == '"') return input_.substr(start, position_ - start);
      if (byte == '\\') {
        if (position_ == input_.size()) break;
        ++position_;
      }
    }
    Fail("unterminated string");
  }

  static auto Key(std::string_view token) -> std::string {
    // Only keys are decoded, so escaped discriminator names and duplicate
    // keys are recognized. Numeric values never enter this parser.
    flatbuffers::Parser parser;
    parser.opts.strict_json = true;
    flexbuffers::Builder builder;
    const std::string quoted(token);
    if (!parser.ParseFlexBuffer(quoted.c_str(), nullptr, &builder))
      Fail(parser.error_);
    auto key = flexbuffers::GetRoot(builder.GetBuffer()).AsString().str();
    if (key.find('\0') != std::string::npos) Fail("NUL byte in object key");
    return key;
  }

  auto Value(size_t depth) -> std::string {
    if (depth > 128) Fail("JSON nesting is too deep");
    Whitespace();
    if (position_ == input_.size()) Fail("missing value");
    if (input_[position_] == '"') return std::string(String());
    if (Take('{')) {
      struct Member {
        bool discriminator;
        std::string text;
      };
      std::vector<Member> members;
      std::unordered_set<std::string> keys;
      if (!Take('}')) {
        do {
          const auto token = String();
          const auto key = Key(token);
          if (!keys.insert(key).second) Fail("duplicate object key");
          if (!Take(':')) Fail("missing colon after object key");
          members.push_back({key.ends_with("_type"),
                             std::string(token) + ":" + Value(depth + 1)});
          if (Take('}')) break;
          if (!Take(',')) Fail("missing comma between object members");
        } while (true);
      }
      std::string output = "{";
      bool first = true;
      for (const bool discriminator : {true, false}) {
        for (const auto& member : members) {
          if (member.discriminator != discriminator) continue;
          if (!first) output += ',';
          first = false;
          output += member.text;
        }
      }
      return output + '}';
    }
    if (Take('[')) {
      std::string output = "[";
      if (!Take(']')) {
        do {
          output += Value(depth + 1);
          if (Take(']')) break;
          if (!Take(',')) Fail("missing comma between array elements");
          output += ',';
        } while (true);
      }
      return output + ']';
    }
    const auto start = position_;
    while (position_ < input_.size() &&
           std::string_view(",}] \t\r\n").find(input_[position_]) ==
               std::string_view::npos) {
      if (input_[position_] == '\0') Fail("embedded NUL byte");
      ++position_;
    }
    if (position_ == start) Fail("missing value");
    return std::string(input_.substr(start, position_ - start));
  }

  std::string_view input_;
  size_t position_ = 0;
};

auto JsonString(const std::string& value) -> std::string {
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') {
      result += '\\';
      result += static_cast<char>(byte);
    } else if (byte < 32) {
      result += "\\u00";
      result += hex[byte >> 4];
      result += hex[byte & 15];
    } else
      result += static_cast<char>(byte);
  }
  result += '"';
  return result;
}

auto Run(int argc, char** argv) -> int {
  C::Options options;
  std::string schema_path;
  std::chrono::milliseconds timeout{60000};
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto next = [&]() -> std::string {
      if (++i >= argc) throw std::runtime_error(argument + " needs a value");
      return argv[i];
    };
    if (argument == "--endpoint")
      options.endpoint = next();
    else if (argument == "--schema")
      schema_path = next();
    else if (argument == "--subprotocol")
      options.subprotocol = next();
    else if (argument == "--timeout-ms")
      timeout = std::chrono::milliseconds(std::stoll(next()));
    else if (argument == "--help") {
      std::cout << "Usage: tsugumi-rpc --endpoint "
                   "unix:/path|ws://host:port/rpc --schema protocol.bfbs\n"
                   "Reads one FlatBuffers request envelope as JSON per line.\n"
                   "Writes reply/event envelopes as JSON lines; request IDs "
                   "must increase.\n";
      return 0;
    } else
      throw std::runtime_error("unknown option " + argument);
  }
  if (options.endpoint.empty() || schema_path.empty())
    throw std::runtime_error("--endpoint and --schema are required");
  if (timeout.count() <= 0)
    throw std::runtime_error("--timeout-ms must be positive");
  std::ifstream file(schema_path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open RPC schema");
  const C::Bytes schema{std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>()};
  flatbuffers::Verifier verifier(schema.data(), schema.size());
  if (!reflection::VerifySchemaBuffer(verifier))
    throw std::runtime_error("invalid RPC schema");
  const auto* reflected = reflection::GetSchema(schema.data());
  if (!reflected->root_table() || !reflected->file_ident())
    throw std::runtime_error("RPC schema has no root or identifier");
  const auto identifier = reflected->file_ident()->str();
  if (identifier == "TSA1")
    options.protocol = C::Protocol::Agent;
  else if (identifier != "TSG1")
    throw std::runtime_error("unsupported RPC schema identifier");
  flatbuffers::Parser encoder;
  flatbuffers::Parser decoder;
  encoder.opts.strict_json = true;
  decoder.opts.strict_json = true;
  decoder.opts.output_default_scalars_in_json = true;
  decoder.opts.indent_step = -1;
  if (!encoder.Deserialize(schema.data(), schema.size()) ||
      !decoder.Deserialize(schema.data(), schema.size()))
    throw std::runtime_error("cannot load RPC schema");
  std::mutex output_mutex;
  std::mutex wait_mutex;
  std::condition_variable changed;
  std::atomic<size_t> pending{0};
  std::atomic<bool> closed{false};
  std::atomic<bool> failed{false};
  const auto print = [&](const C::Bytes& bytes) {
    std::lock_guard lock(output_mutex);
    std::string json;
    if (const auto* error =
            flatbuffers::GenerateText(decoder, bytes.data(), &json)) {
      failed = true;
      std::cout << "{\"transport_error\":{\"code\":3,\"message\":"
                << JsonString(error) << "}}\n"
                << std::flush;
      return;
    }
    std::cout << json << '\n' << std::flush;
  };
  // Captured callbacks outlive Connection, including during exception
  // unwinding.
  C::Connection connection(options);
  connection.OnEvent(print);
  connection.OnClosed([&](C::Error error) {
    if (error.code != C::ErrorCode::Closed) failed = true;
    {
      std::lock_guard lock(wait_mutex);
      closed = true;
    }
    changed.notify_all();
  });
  if (const auto error = connection.Start().get())
    throw std::runtime_error(error.message);
  std::string input;
  bool eof = false;
  while (!closed && !eof) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, 100);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("cannot poll stdin");
    }
    if (!ready) continue;
    char block[8192];
    const auto count = ::read(STDIN_FILENO, block, sizeof block);
    if (count < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      throw std::runtime_error("cannot read stdin");
    }
    if (!count)
      eof = true;
    else
      input.append(block, static_cast<size_t>(count));
    if (input.size() > options.max_frame_bytes * 8)
      throw std::runtime_error("JSON request line is too large");
    while (true) {
      const auto newline = input.find('\n');
      if (newline == std::string::npos && !eof) break;
      auto line = input.substr(0, newline);
      input.erase(0, newline == std::string::npos ? input.size() : newline + 1);
      if (line.find_first_not_of(" \t\r") != std::string::npos) {
        const auto ordered = JsonOrder(line).Run();
        if (!encoder.ParseJson(ordered.c_str()))
          throw std::runtime_error("invalid request JSON: " + encoder.error_);
        auto* begin = encoder.builder_.GetBufferPointer();
        C::Bytes frame(begin, begin + encoder.builder_.GetSize());
        {
          std::lock_guard lock(wait_mutex);
          ++pending;
        }
        connection.CallFrame(
            std::move(frame),
            [&](C::Result result) {
              if (result.error) {
                failed = true;
                std::lock_guard lock(output_mutex);
                std::cout << "{\"transport_error\":{\"code\":"
                          << static_cast<int>(result.error.code)
                          << ",\"message\":" << JsonString(result.error.message)
                          << "}}\n"
                          << std::flush;
              } else
                print(result.bytes);
              {
                std::lock_guard lock(wait_mutex);
                --pending;
              }
              changed.notify_all();
            },
            timeout);
      }
      if (input.empty()) break;
    }
  }
  {
    std::unique_lock lock(wait_mutex);
    changed.wait(lock, [&] { return pending == 0 || closed; });
  }
  connection.Close();
  return failed ? 1 : 0;
}
}  // namespace

auto main(int argc, char** argv) -> int {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "tsugumi-rpc: " << error.what() << '\n';
    return 1;
  }
}
