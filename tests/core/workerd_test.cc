// Socket-free protocol and journal tests. The Linux integration exercises the
// same binary's listeners, namespace launcher and real Nix/Card execution.
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <type_traits>

#include "journal.hh"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/cache_publish.hh"
#include "tsugumi/workerd/log.hh"
#include "tsugumi/workerd/log_limits.hh"
#include "tsugumi/workerd/path_ids.hh"

namespace tsugumi::workerd {
namespace {
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
auto spec(const std::string& digest, uint64_t build_timeout_ms = 60,
          uint64_t log_limit = 8u << 20) -> std::string {
  R::JobSpecT s;
  s.system = "x86_64-linux";
  s.derivation = "/nix/store/00000000000000000000000000000000-test.drv";
  s.output = "/nix/store/00000000000000000000000000000000-test";
  s.invocation_sha256 = digest;
  s.input_blobs = {digest};
  s.build_timeout_ms = build_timeout_ms;
  s.log_limit_bytes = log_limit;
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
auto test_id(char digit) -> Sha256Hex {
  return Sha256Hex::parse(std::string(64, digit)).value();
}

auto put_blob(Journal& journal, const Sha256Hex& job,
              const R::PutBlobRequestT& chunk, std::string_view core) -> void {
  journal.put(job, Sha256Hex::parse(chunk.digest).value(), chunk.total,
              chunk.offset, chunk.bytes, core);
}

auto get_blob(const Journal& journal, const Sha256Hex& job,
              const R::GetBlobRequestT& request, std::string_view core)
    -> R::BlobChunkT {
  return journal.get(job, Sha256Hex::parse(request.digest).value(),
                     request.offset, request.limit, core);
}

auto submit(Journal& journal, const Sha256Hex& job, std::string_view bytes,
            std::string_view core) -> Record {
  return journal.submit(job, bytes, parse_input_blobs(*parse_spec(bytes)),
                        core);
}

static_assert(!std::is_invocable_v<decltype(&Journal::job_path), const Journal&,
                                   std::string_view>);
static_assert(!std::is_invocable_v<decltype(&Journal::supervisor_lock_path),
                                   const Journal&, std::string_view>);
static_assert(!std::is_invocable_v<decltype(&Journal::get), const Journal&,
                                   const Sha256Hex&, std::string_view, uint64_t,
                                   uint32_t, std::string_view>);

auto path_id_test() -> void {
  const std::string digest = crypto::sha256_hex("input");
  const auto parsed = Sha256Hex::parse(digest);
  expect(parsed && parsed->view() == digest, "canonical digest was rejected");
  expect(!Sha256Hex::parse(digest.substr(1)), "short digest was accepted");
  expect(!Sha256Hex::parse(std::string(64, 'A')),
         "uppercase digest was accepted");
  auto job_spec = parse_spec(spec(digest));
  job_spec->input_blobs.push_back("missing");
  try {
    parse_input_blobs(*job_spec);
  } catch (const Error& error) {
    expect(error.code == R::ErrorCode::Invalid, "unexpected blob ID error");
    return;
  }
  throw std::runtime_error("invalid input blob digest was accepted");
}

auto log_test() -> void {
  InvocationLog sample(kInvocationLogLimitBytes);
  const auto build = sample.append(LogStream::build, "build");
  const auto error = sample.append(LogStream::output, "\n");
  expect(build && error, "sample entries omitted");
  const auto build_size = build->size();
  const auto error_size = error->size();
  InvocationLog maximum(build_size + error_size);
  expect(maximum.append(LogStream::build, "build").has_value(),
         "build entry omitted");
  expect(maximum.append(LogStream::output, "\n").has_value(),
         "exact fit was rejected");
  expect(!maximum.truncated(), "an exact fit was marked truncated");
  expect(!maximum.append(LogStream::output, "overflow") &&
             maximum.truncated(),
         "omitted entry was not marked truncated");
  expect(!maximum.append(LogStream::journal, "journal"),
         "wrote an entry after truncation");

  InvocationLog short_frame(error_size - 1);
  expect(
      !short_frame.append(LogStream::output, "\n") &&
          short_frame.truncated(),
      "budget did not account for the complete size-prefixed entry");

  InvocationLog combined(build_size + error_size - 1);
  expect(combined.append(LogStream::build, "build").has_value(),
         "build entry omitted");
  expect(
      !combined.append(LogStream::output, "\n") && combined.truncated(),
      "combined stream budget was not enforced");
}
auto protocol_test() -> void {
  const std::string digest(64, 'a');
  expect(parse_spec(spec(digest, 60, 17))->log_limit_bytes == 17,
         "job lost the core log budget");
  R::RequestT q;
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
             request->project_scope == q.project_scope,
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
  const std::string core(64, 'a');
  Journal other(root / "b");
  expect(j.instance() != other.instance(), "project identities overlap");
  expect(Journal(root / "a").instance() == j.instance(),
         "identity did not survive restart");
  const auto job = test_id('1'), second = test_id('2'),
             cancelled = test_id('3');
  auto p = chunk("job input");
  auto bytes = spec(p.digest);
  const auto fingerprint = crypto::sha256_hex(bytes);
  expect(j.status(job).state == R::JobState::Missing,
         "unknown job must be missing");
  rejected([&] { submit(j, job, bytes, core); });
  put_blob(j, job, p, core);
  put_blob(j, job, p, core);
  R::GetBlobRequestT get;
  get.digest = p.digest;
  expect(get_blob(j, job, get, core).bytes == p.bytes, "blob roundtrip");
  rejected([&] { get_blob(other, job, get, core); });
  auto corrupt = p;
  corrupt.digest = std::string(64, 'a');
  rejected([&] { put_blob(j, job, corrupt, core); });
  corrupt = p;
  corrupt.total = kMaxBlob + 1;
  rejected([&] { put_blob(j, job, corrupt, core); });
  corrupt = p;
  corrupt.offset = 1;
  rejected([&] { put_blob(j, second, corrupt, core); });
  auto large = chunk(std::string(kChunkBytes + 17, 'x'));
  auto tail = large;
  large.bytes.resize(kChunkBytes);
  put_blob(j, job, large, core);
  put_blob(j, job, large, core);  // repeat an already acknowledged chunk
  tail.offset = kChunkBytes;
  tail.bytes.erase(tail.bytes.begin(), tail.bytes.begin() + kChunkBytes);
  put_blob(j, job, tail, core);
  get.digest = large.digest;
  get.offset = kChunkBytes;
  expect(get_blob(j, job, get, core).bytes == tail.bytes,
         "chunked binary transfer");
  get.limit = kChunkBytes + 1;
  rejected([&] { get_blob(j, job, get, core); });
  auto accepted = submit(j, job, bytes, core);
  expect(accepted.state == R::JobState::Queued &&
             accepted.fingerprint == fingerprint,
         "submit receipt");
  expect(submit(j, job, bytes, core).fingerprint == accepted.fingerprint,
         "resubmission changed the receipt");
  rejected([&] { submit(j, job, spec(p.digest, 61), core); });
  rejected([&] { j.cancel(job, std::string(64, 'f')); });
  rejected([&] { put_blob(j, job, p, core); });
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
  expect(submit(j, job, bytes, core).state == R::JobState::Released,
         "release allowed a late replay");
  j.cancel(cancelled, fingerprint);
  expect(submit(j, cancelled, bytes, core).state == R::JobState::Cancelled,
         "cancel-before-submit allowed execution");
  rejected([&] { submit(j, cancelled, spec(p.digest, 62), core); });
  put_blob(j, second, p, core);
  submit(j, second, bytes, core);
  Journal reopened(root / "a");
  expect(reopened.status(second).state == R::JobState::Queued,
         "queue lost on reopen");
  expect(reopened.claim(second, 1, 1, "boot"),
         "reopened queue cannot be claimed");
  reopened.finish(second, R::JobState::Interrupted, {}, "supervisor lost");
  expect(!reopened.claim(second, 1, 1, "boot"),
         "interrupted invocation was rerun");
  const auto queued = test_id('5');
  put_blob(j, queued, p, core);
  submit(j, queued, bytes, core);
  rejected([&] { j.discard(queued, false); });
  expect(j.discard(queued, true).state == R::JobState::Queued &&
             j.status(queued).state == R::JobState::Released &&
             j.status(queued).fingerprint == fingerprint &&
             !j.claim(queued, 1, 1, "boot"),
         "removed queued job could still start");
  expect(j.discard(second, false).state == R::JobState::Interrupted &&
             std::distance(stdfs::directory_iterator(j.job_path(second)),
                           stdfs::directory_iterator{}) == 1,
         "removal kept payload");
  rejected([&] { put_blob(j, second, p, core); });
  rejected([&] { j.discard(test_id('6'), true); });
  const auto result_job = test_id('7');
  put_blob(j, result_job, p, core);
  submit(j, result_job, bytes, core);
  j.claim(result_job, 100, 1, "boot");
  expect(j.log(result_job, 0, kLogChunkBytes, core) ==
             std::pair<uint64_t, std::string>{},
         "a job without a log reported bytes");
  const auto output = j.job_path(result_job) / "work/result";
  private_directory(output);
  // A running job's log is read from the core's cursor, in bounded slices,
  // by its own core life only.
  fs::write_atomic(output / "logs", "running entry\npart", 0600);
  expect(j.log(result_job, 0, 8, core) ==
             std::pair<uint64_t, std::string>{18, "running "},
         "log slice from the start");
  expect(j.log(result_job, 14, kLogChunkBytes, core).second == "part",
         "log slice from a cursor");
  expect(j.log(result_job, 18, kLogChunkBytes, core) ==
             std::pair<uint64_t, std::string>{18, ""},
         "a cursor at the end returned bytes");
  expect(j.log(result_job, 40, kLogChunkBytes, core).second.empty(),
         "a cursor past the end returned bytes");
  rejected([&] { j.log(result_job, 0, kLogChunkBytes + 1, core); });
  rejected([&] { j.log(result_job, 0, 8, std::string(64, 'b')); });
  stdfs::remove(output / "logs");
  stdfs::create_symlink(j.job_path(result_job) / "spec.fb",
                        output / "logs");
  rejected([&] { j.log(result_job, 0, 8, core); });
  stdfs::remove(output / "logs");
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
  fs::write_atomic(output / "logs", "job log", 0600);
  stdfs::create_symlink("logs", output / "link");
  rejected([&] { j.import_results(result_job); });
  stdfs::remove(output / "link");
  fs::write_atomic(output.parent_path() / "artifact-wanted", "stale request",
                   0600);
  put_blob(j, result_job, p, core);
  const auto result = j.import_results(result_job);
  rejected([&] { put_blob(j, result_job, p, core); });
  R::GetBlobRequestT manifest_request;
  manifest_request.digest = result;
  auto manifest_data = get_blob(j, result_job, manifest_request, core);
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
  // An acknowledged prefix can be replayed after reopening. A crash after
  // writing the tail but before syncing or publishing it must leave the blob
  // invisible until the tail is replayed and the whole blob verified.
  const auto interrupted_upload = test_id('8');
  auto partial = chunk("prefixsuffix");
  auto prefix = partial;
  prefix.bytes.resize(6);
  put_blob(j, interrupted_upload, prefix, core);
  put_blob(reopened, interrupted_upload, prefix, core);
  {
    std::ofstream tail(
        j.job_path(interrupted_upload) / "parts" / partial.digest,
        std::ios::app);
    tail << "suffix";
  }
  auto suffix = partial;
  suffix.offset = 6;
  suffix.bytes.erase(suffix.bytes.begin(), suffix.bytes.begin() + 6);
  R::GetBlobRequestT repaired;
  repaired.digest = partial.digest;
  rejected([&] { get_blob(reopened, interrupted_upload, repaired, core); });
  const auto interrupted_spec = spec(partial.digest);
  rejected(
      [&] { submit(reopened, interrupted_upload, interrupted_spec, core); });
  put_blob(reopened, interrupted_upload, suffix, core);
  expect(
      get_blob(Journal(root / "a"), interrupted_upload, repaired, core).bytes ==
          partial.bytes,
      "last-chunk retry did not publish the blob");
  // A crashed release leaves the tombstone before payload deletion; recovery
  // completes it. No run data are promoted into the persistent store.
  fs::write_atomic(j.job_path(job) / "leftover", "data", 0600);
  reopened.clean_released(job);
  expect(!stdfs::exists(j.job_path(job) / "leftover"),
         "release recovery did not erase scratch");
  auto insecure = root / "insecure";
  private_directory(insecure);
  ::chmod(insecure.c_str(), 0755);
  rejected([&] { Journal bad(insecure); });
}
auto timeout_budget_test(const stdfs::path& root) -> void {
  Journal journal(root / "timeouts");
  const std::string core(64, 'b');
  const auto job = test_id('e');
  const auto input = chunk("input");
  put_blob(journal, job, input, core);
  auto bytes = spec(input.digest, 0);
  auto settings = parse_spec(bytes);
  settings->build_timeout_ms = 1;
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(R::JobSpec::Pack(builder, settings.get()));
  bytes.assign(reinterpret_cast<const char*>(builder.GetBufferPointer()),
               builder.GetSize());
  journal.clean_released(job);
  expect(journal.status(job).state == R::JobState::Uploading,
         "cleanup discarded a staged upload");
  expect(submit(journal, job, bytes, core).state == R::JobState::Queued,
         "submission did not queue the job");
  expect(journal.claim(job, 1, 1, "boot"),
         "queue consumed a Card or build budget");
  auto decoded = parse_spec(bytes);
  expect(decoded->build_timeout_ms == 1, "build budget was not preserved");
}

auto cache_publish_credentials_test(const stdfs::path& root) -> void {
  const auto tools = root / "upload-tools";
  private_directory(tools);
  const auto fake = tools / "cachix";
  fs::write_atomic(fake, R"script(#!/bin/sh
test "$#" -eq 3 && test "$1" = push && test "$2" = fixture || exit 2
test "$3" = /nix/store/example && test "$CACHIX_AUTH_TOKEN" = synthetic-cache-token || exit 3
printf '%s\n' "invoked" > "${0}.ran"
printf '%s\n' "$CACHIX_AUTH_TOKEN"
printf '%s\n' "$CACHIX_AUTH_TOKEN" >&2
exit 1
)script",
                   0700);
  expect(::setenv("PATH", tools.c_str(), 1) == 0, "could not set test path");
  std::string failure;
  try {
    publish_to_external_cache(
        {ExternalCacheKind::cachix, {}, "fixture", {}, "synthetic-cache-token"},
        root, {}, {"/nix/store/example"}, {});
  } catch (const std::exception& error) {
    failure = error.what();
  }
  expect(stdfs::exists(fake.string() + ".ran"),
         "uploader did not receive the token in its environment alone");
  expect(failure == "binary cache upload failed",
         "uploader diagnostic escaped the controlled error message");
  for (const auto& entry : stdfs::directory_iterator(root))
    expect(!entry.path().filename().string().starts_with("cache-publish."),
           "cache publisher left a private configuration directory");
}
}  // namespace
}  // namespace tsugumi::workerd

auto main(int argc, char** argv) -> int {
  using namespace tsugumi::workerd;
  ::umask(0077);
  const auto root = stdfs::temp_directory_path() /
                    ("tsugumi-workerd-" + tsugumi::crypto::uuid4());
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--timeout-budgets") {
      timeout_budget_test(root);
      discard_tree(root);
      return 0;
    }
    log_test();
    protocol_test();
    path_id_test();
    journal_test(root);
    cache_publish_credentials_test(root);
    discard_tree(root);
    std::cout << "workerd protocol and journal tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    discard_tree(root);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
