// Socket-free protocol and journal tests. The Linux integration exercises the
// same binary's listeners, namespace launcher and real Nix/Card execution.
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

#include "journal.hh"
#include "tsugumi/workerd/log.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi::workerd {
namespace {
template<class T> concept HasWorkerToken = requires(T value) { value.token; };
static_assert(!HasWorkerToken<R::RequestT>);
auto expect(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}
template <typename F>
auto rejected(F f) -> void {
  bool failed = false;
  try {
    f();
  } catch (const std::exception&) {
    failed = true;
  }
  expect(failed, "invalid input was accepted");
}
auto spec(const std::string& digest, uint32_t timeout = 60) -> std::string {
  R::JobSpecT s;
  s.system = "x86_64-linux";
  s.derivation = "/nix/store/00000000000000000000000000000000-test.drv";
  s.output = "/nix/store/00000000000000000000000000000000-test";
  s.invocation_sha256 = digest;
  s.input_blobs = {digest};
  s.timeout_seconds = timeout;
  validate_spec(s);
  flatbuffers::FlatBufferBuilder b;
  b.Finish(R::JobSpec::Pack(b, &s));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}
auto chunk(const std::string& data) -> R::PutBlobRequestT {
  R::PutBlobRequestT p;
  p.digest = crypto::sha256_hex(data);
  p.total = data.size();
  p.bytes.assign(data.begin(), data.end());
  return p;
}
auto log_test() -> void {
  expect(kDefaultLogLimitBytes == 8 * 1024 * 1024, "default log limit changed");
  expect(parse_log_limit_bytes("23") == 23, "log limit setting was not parsed");
  expect(parse_log_limit_bytes("18446744073709551615") == UINT64_MAX,
         "log limit setting lost precision");
  for (const auto* value : {"", "0", "-1", "+1", " 1", "23bytes", "18446744073709551616"})
    rejected([&] { parse_log_limit_bytes(value); });
  // 25 encoded bytes: the raw text fits, but JSON escaping and framing do not
  // fit in a 23-byte budget. A dropped entry must not look like a complete log.
  std::ostringstream output;
  InvocationLog log(output, 23);
  log.append(1, "\n");
  log.finish();
  remote::LogJson entry;
  std::istringstream lines(output.str());
  std::string line;
  expect(static_cast<bool>(std::getline(lines, line)), "missing truncation notice");
  expect(!schema::decode(line, entry) && *entry.stream == 3 &&
             *entry.text == "\n[Log output truncated at the 23-byte limit.]\n",
         "invalid truncation notice");
  expect(!std::getline(lines, line), "unexpected log entry");

  const std::string first = "{\"stream\":4,\"text\":\"build\"}\n";
  const std::string second = "{\"stream\":2,\"text\":\"error\"}\n";
  for (const auto limit : {first.size() + second.size(), first.size() + second.size() + 1}) {
    std::ostringstream complete;
    InvocationLog full(complete, limit);
    full.append(4, "build");
    full.append(2, "error");
    full.finish();
    expect(complete.str() == first + second, "complete log was changed or marked truncated");
  }
  std::ostringstream combined;
  InvocationLog bounded(combined, first.size() + second.size() - 1);
  bounded.append(4, "build");
  bounded.append(2, "error");
  bounded.append(1, std::string(100, 'x'));
  bounded.finish();
  const auto once = combined.str();
  bounded.finish();
  expect(combined.str() == once, "duplicate truncation notice");
  expect(once.starts_with(first) && once.find("error") == std::string::npos,
         "combined stream budget was not enforced");
  expect(once.find("[Log output truncated at the " + std::to_string(first.size() + second.size() - 1) +
                       "-byte limit.]") != std::string::npos,
         "combined stream truncation lacks its configured limit");

  std::ostringstream tiny;
  InvocationLog minimum(tiny, 1);
  minimum.append(1, "too large");
  minimum.append(2, "also too large");
  minimum.finish();
  expect(tiny.str().find("[Log output truncated at the 1-byte limit.]") != std::string::npos,
         "tiny budget suppressed the truncation notice");
}
auto protocol_test() -> void {
  R::RequestT q;
  q.server_id = std::string(64, 'b');
  q.project_scope = std::string(64, 'c');
  auto c = chunk(std::string("binary\0payload", 14));
  q.body.Set(c);
  R::MessageT m;
  m.payload.Set(q);
  const auto bytes = encode(std::move(m));
  const auto prefix = length_prefix(static_cast<uint32_t>(bytes.size()));
  expect(frame_length(prefix) == bytes.size(),
         "little-endian frame length roundtrip");
  expect(static_cast<uint8_t>(prefix[0]) == (bytes.size() & 255),
         "wrong byte order");
  auto restored = decode(bytes);
  const auto* request = restored->payload.AsRequest();
  expect(request && request->body.AsPutBlobRequest()->bytes == c.bytes &&
             request->server_id == q.server_id,
         "protocol roundtrip");
  for (const auto& data :
       {std::string{}, bytes.substr(0, 11), std::string(32, '\0')})
    rejected([&] { decode(data); });
  rejected([] { frame_length(length_prefix(kMaxFrame + 1)); });
  rejected([] { frame_length(length_prefix(0)); });
  auto failure_message =
      decode(encode(failure(R::ErrorCode::InstanceChanged, "changed")));
  expect(failure_message->payload.AsReply()->body.AsFailure()->code ==
             R::ErrorCode::InstanceChanged,
         "failure roundtrip");
}
auto journal_test(const stdfs::path& root) -> void {
  Journal j(root / "a");
  Journal other(root / "b");
  expect(j.instance() != other.instance(), "project identities overlap");
  expect(Journal(root / "a").instance() == j.instance(),
         "identity did not survive restart");
  const std::string job(64, '1'), second(64, '2'), cancelled(64, '3');
  auto p = chunk("job input");
  auto bytes = spec(p.digest);
  const auto fingerprint = crypto::sha256_hex(bytes);
  expect(j.status(job).state == R::JobState::Missing,
         "unknown job must be missing");
  rejected([&] { j.status("../escape"); });
  rejected([&] { j.submit(job, bytes); });
  j.put(job, p);
  j.put(job, p);
  R::GetBlobRequestT get;
  get.digest = p.digest;
  expect(j.get(job, get).bytes == p.bytes, "blob roundtrip");
  rejected([&] { other.get(job, get); });
  auto corrupt = p;
  corrupt.digest = std::string(64, 'a');
  rejected([&] { j.put(job, corrupt); });
  corrupt = p;
  corrupt.total = kMaxBlob + 1;
  rejected([&] { j.put(job, corrupt); });
  corrupt = p;
  corrupt.offset = 1;
  rejected([&] { j.put(second, corrupt); });
  auto large = chunk(std::string(kChunkBytes + 17, 'x'));
  auto tail = large;
  large.bytes.resize(kChunkBytes);
  j.put(job, large);
  j.put(job, large);  // repeat an already acknowledged chunk
  tail.offset = kChunkBytes;
  tail.bytes.erase(tail.bytes.begin(), tail.bytes.begin() + kChunkBytes);
  j.put(job, tail);
  get.digest = large.digest;
  get.offset = kChunkBytes;
  expect(j.get(job, get).bytes == tail.bytes, "chunked binary transfer");
  get.limit = kChunkBytes + 1;
  rejected([&] { j.get(job, get); });
  auto accepted = j.submit(job, bytes);
  expect(accepted.state == R::JobState::Queued &&
             accepted.fingerprint == fingerprint,
         "submit receipt");
  expect(j.submit(job, bytes).deadline == accepted.deadline,
         "resubmission reset deadline");
  rejected([&] { j.submit(job, spec(p.digest, 61)); });
  rejected([&] { j.cancel(job, std::string(64, 'f')); });
  rejected([&] { j.put(job, p); });
  rejected([&] { j.release(job, fingerprint); });
  std::atomic<int> claims{0};
  std::vector<std::thread> threads;
  for (int n = 0; n < 12; ++n)
    threads.emplace_back([&] {
      if (Journal(root / "a").claim(job, 100, 1, "boot")) ++claims;
    });
  for (auto& t : threads) t.join();
  expect(claims == 1 && !j.claim(job, 101, 2, "boot"), "job was claimed twice");
  expect(j.cancel(job, fingerprint).state == R::JobState::Cancelling,
         "running cancellation");
  j.finish(job, R::JobState::Succeeded, std::string(64, '4'), {});
  expect(j.status(job).state == R::JobState::Cancelled &&
             j.status(job).result.empty(),
         "cancellation must win terminal race");
  j.release(job, fingerprint);
  j.release(job, fingerprint);
  expect(j.status(job).state == R::JobState::Released &&
             !stdfs::exists(j.job_path(job) / "blobs"),
         "release kept run data");
  expect(std::distance(stdfs::directory_iterator(j.job_path(job)),
                       stdfs::directory_iterator{}) == 1,
         "tombstone kept payload");
  expect(j.submit(job, bytes).state == R::JobState::Released,
         "release allowed a late replay");
  j.cancel(cancelled, fingerprint);
  expect(j.submit(cancelled, bytes).state == R::JobState::Cancelled,
         "cancel-before-submit allowed execution");
  rejected([&] { j.submit(cancelled, spec(p.digest, 62)); });
  j.put(second, p);
  j.submit(second, bytes);
  Journal reopened(root / "a");
  expect(reopened.status(second).state == R::JobState::Queued,
         "queue lost on reopen");
  expect(reopened.claim(second, 1, 1, "boot"),
         "reopened queue cannot be claimed");
  reopened.finish(second, R::JobState::Interrupted, {}, "supervisor lost");
  expect(!reopened.claim(second, 1, 1, "boot"),
         "interrupted invocation was rerun");
  const std::string result_job(64, '7');
  j.put(result_job, p);
  j.submit(result_job, bytes);
  j.claim(result_job, 100, 1, "boot");
  const auto output = j.job_path(result_job) / "work/result";
  private_directory(output);
  flatbuffers::FlatBufferBuilder worker;
  namespace W = Tsugumi::Worker;
  const auto values =
      worker.CreateVector(std::vector<flatbuffers::Offset<W::WorkerOutput>>{});
  const auto value = W::CreateWorkerResult(worker, values);
  W::FinishWorkerMessageBuffer(
      worker, W::CreateWorkerMessage(worker, 1, W::WorkerPayload::WorkerResult,
                                     value.Union()));
  fs::write_atomic(output / "worker.fb",
                   {reinterpret_cast<const char*>(worker.GetBufferPointer()),
                    worker.GetSize()},
                   0600);
  fs::write_atomic(output / "logs.jsonl", "job log", 0600);
  stdfs::create_symlink("logs.jsonl", output / "link");
  rejected([&] { j.import_results(result_job); });
  stdfs::remove(output / "link");
  const auto result = j.import_results(result_job);
  R::GetBlobRequestT manifest_request;
  manifest_request.digest = result;
  auto manifest_data = j.get(result_job, manifest_request);
  flatbuffers::Verifier verifier(manifest_data.bytes.data(),
                                 manifest_data.bytes.size());
  expect(verifier.VerifyBuffer<R::ResultManifest>(nullptr),
         "imported result manifest is invalid");
  j.finish(result_job, R::JobState::Succeeded, result, {});
  expect(j.status(result_job).result == result,
         "result reference was not durable");
  j.release(result_job, fingerprint);
  expect(!stdfs::exists(j.job_path(result_job) / "work"),
         "release retained result workspace");
  // A crash after fsync of the last chunk but before publication must be
  // repairable by retransmitting that same chunk.
  const std::string interrupted_upload(64, '8');
  auto partial = chunk("prefixsuffix");
  auto prefix = partial;
  prefix.bytes.resize(6);
  j.put(interrupted_upload, prefix);
  fs::write_atomic(j.job_path(interrupted_upload) / "parts" / partial.digest,
                   "prefixsuffix", 0600);
  auto suffix = partial;
  suffix.offset = 6;
  suffix.bytes.erase(suffix.bytes.begin(), suffix.bytes.begin() + 6);
  j.put(interrupted_upload, suffix);
  R::GetBlobRequestT repaired;
  repaired.digest = partial.digest;
  expect(j.get(interrupted_upload, repaired).bytes == partial.bytes,
         "last-chunk retry did not publish the blob");
  // Malformed specifications and optional limit pairing.
  rejected([&] { parse_spec("{}"); });
  auto s = parse_spec(bytes);
  s->cpu_millis = 500;
  rejected([&] { validate_spec(*s); });
  s->memory_bytes = uint64_t{1} << 41;
  rejected([&] { validate_spec(*s); });
  s->memory_bytes = 1 << 20;
  validate_spec(*s);
  s->input_blobs.push_back(p.digest);
  rejected([&] { validate_spec(*s); });
  s->input_blobs.pop_back();
  s->output += "/escape";
  rejected([&] { validate_spec(*s); });
  // A crashed release leaves the tombstone before payload deletion; recovery
  // completes it. No run data are promoted into the persistent store.
  fs::write_atomic(j.job_path(job) / "leftover", "data", 0600);
  reopened.expire(job);
  expect(!stdfs::exists(j.job_path(job) / "leftover"),
         "release recovery did not erase scratch");
  auto insecure = root / "insecure";
  private_directory(insecure);
  ::chmod(insecure.c_str(), 0755);
  rejected([&] { Journal bad(insecure); });
}
}  // namespace
}  // namespace tsugumi::workerd

auto main() -> int {
  using namespace tsugumi::workerd;
  ::umask(0077);
  const auto root = stdfs::temp_directory_path() /
                    ("tsugumi-workerd-" + tsugumi::crypto::uuid4());
  try {
    log_test();
    protocol_test();
    journal_test(root);
    discard_tree(root);
    std::cout << "workerd protocol and journal tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    discard_tree(root);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
