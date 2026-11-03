// Behaviour tests of the core without RPC: port types, graph resolution and
// the Nix upstream analysis, run plans, history state selection and the
// notification send queue.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "graph.hh"
#include "history.hh"
#include "scheduler.hh"
#include "server.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "types.hh"
#include "util/crypto.hh"

using namespace tsugumi;
namespace stdfs = std::filesystem;

// ---- harness ------------------------------------------------------------------

namespace {

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& cases() {
  static std::vector<Case> v;
  return v;
}
struct Reg {
  Reg(const char* n, void (*f)()) { cases().push_back({n, f}); }
};
int g_failures = 0;
void fail(const char* file, int line, const std::string& what) {
  std::fprintf(stderr, "  %s:%d: %s\n", file, line, what.c_str());
  ++g_failures;
}

#define TEST(name)                         \
  static void name();                      \
  static Reg reg_##name(#name, &name);     \
  static void name()
#define CHECK(c) \
  do {           \
    if (!(c)) fail(__FILE__, __LINE__, #c); \
  } while (0)
#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    auto _a = (a);                                                                       \
    auto _b = (b);                                                                       \
    if (!(_a == _b)) fail(__FILE__, __LINE__, std::string(#a " == " #b));               \
  } while (0)

stdfs::path temp_dir() {
  auto p = stdfs::temp_directory_path() / ("tsugumi-core-test-" + crypto::uuid4());
  stdfs::create_directories(p);
  return p;
}

void write(const stdfs::path& p, const std::string& text) {
  stdfs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

const char* kNumberFbs = R"(namespace t; table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;)";
const char* kFlagFbs = R"(namespace t; table Flag { value:bool; } table Outputs { flag:Flag (required); } root_type Outputs;)";

// Cards named for a definition written under another name (c("go",
// "trigger")): each Card gets its own copy when the project is resolved.
std::vector<std::pair<std::string, std::string>> g_copies;

// A project on disk built from a list of code / builtin definitions.
struct Project {
  stdfs::path root = temp_dir();
  ~Project() {
    std::error_code ec;
    stdfs::remove_all(root, ec);
  }
  // Code Card with the given inputs ("name" or "name?" for optional); `nix`
  // is the default.nix body expression.
  void code(const std::string& id, std::vector<std::string> inputs, const std::string& nix = "{ run = \"true\"; }",
            const std::string& mode = "all") {
    std::string ins;
    for (auto& i : inputs) {
      bool opt = !i.empty() && i.back() == '?';
      if (opt) i.pop_back();
      ins += std::string(ins.empty() ? "" : ",") + "{\"name\":\"" + i + "\"" + (opt ? ",\"optional\":true" : "") + "}";
    }
    write(root / "definitions" / id / "_card.json",
          "{\"language\":\"bash\",\"trigger\":\"" + std::string(mode == "any" ? "eachInput" : "allInputs") +
              "\",\"inputs\":[" + ins + "]}");
    write(root / "definitions" / id / "_outputs.fbs", kNumberFbs);
    write(root / "definitions" / id / "default.nix", "{ pkgs, inputs, params }:\n" + nix + "\n");
  }
  void builtin(const std::string& id, const std::string& json) { write(root / "definitions" / id / "_card.json", json); }
  void flag(const std::string& id) {
    write(root / "definitions" / id / "_card.json",
          R"({"template":"input","params":[{"name":"value","type":"bool","data":true}]})");
    write(root / "definitions" / id / "_outputs.fbs", kFlagFbs);
  }
  // A Card in Card: the named definitions move into externals/<id>/, a
  // project of its own with `boundary`, linked from definitions/<id>/.
  void external(const std::string& id, const std::vector<std::string>& cards, const std::string& conns,
                const std::string& boundary) {
    auto ext = root / "externals" / id;
    stdfs::create_directories(ext / "definitions");
    std::string list;
    for (const auto& card : cards) {
      stdfs::rename(root / "definitions" / card, ext / "definitions" / card);
      list += std::string(list.empty() ? "" : ",") + "{\"id\":\"" + card + "\"}";
    }
    write(ext / "tsugumi.json", R"({"name":")" + id + R"(","pkgsPath":"tsugumi:default","databaseConnections":[],"canvas":{"cards":[)" +
                                    list + "],\"connections\":[" + conns + "]},\"boundary\":" + boundary + "}");
    write(root / "definitions" / id / "_link.json", "{\"external\":\"" + id + "\"}");
  }
  std::shared_ptr<const Resolution> resolve(const std::string& cards, const std::string& conns) {
    for (const auto& [def, id] : g_copies)
      if (def != id && !stdfs::exists(root / "definitions" / id))
        stdfs::copy(root / "definitions" / def, root / "definitions" / id, stdfs::copy_options::recursive);
    g_copies.clear();
    write(root / "tsugumi.json", R"({"name":"t","pkgsPath":"tsugumi:default","databaseConnections":[],"canvas":{"cards":[)" +
                                     cards + "],\"connections\":[" + conns + "]}}");
    write(root / "layout.json", "{}");
    auto doc = load_from_disk(root);
    auto r = root;
    return tsugumi::resolve(*doc, [r](const std::string& rel) -> std::optional<std::string> {
      std::ifstream f(r / rel, std::ios::binary);
      if (!f) return std::nullopt;
      return std::string(std::istreambuf_iterator<char>(f), {});
    });
  }
};

std::string c(const std::string& id, const std::string& def) {
  g_copies.emplace_back(def, id);
  return "{\"id\":\"" + id + "\"}";
}
std::string e(const std::string& a, const std::string& ap, const std::string& b, const std::string& bp) {
  return R"({"from":{"card":")" + a + R"(","port":")" + ap + R"("},"to":{"card":")" + b + R"(","port":")" + bp + R"("}})";
}

std::set<std::string> guaranteed(const Resolution& r, const std::string& key) {
  std::set<std::string> out;
  for (size_t i : r.guaranteed.at(r.index.at(key))) out.insert(path_key(r.nodes[i].path));
  return out;
}
std::set<std::string> nix_refs(const Resolution& r, const std::string& key) {
  std::set<std::string> out;
  for (size_t i : r.nix_upstream(r.index.at(key))) out.insert(path_key(r.nodes[i].path));
  return out;
}
std::set<std::string> codes(const Resolution& r, const std::string& key) {
  std::set<std::string> out;
  for (const auto& d : r.nodes[r.index.at(key)].diags) out.insert(d.code);
  return out;
}

}  // namespace

// ---- types ----------------------------------------------------------------------

TEST(catalog_ports_and_errors) {
  auto r = types::parse_catalog(
      R"(namespace a; table N { v:int; } table F { value:string; }
         table Outputs { n:N (required); f:F (tsugumi: "file"); } root_type Outputs; attribute "tsugumi";)",
      "x.fbs");
  CHECK(r.errors.empty());
  CHECK_EQ(r.ports.size(), size_t(2));
  CHECK(!r.ports[0].optional && r.ports[1].optional);
  CHECK(r.ports[1].kind == types::PortKind::File);
  auto bad = types::parse_catalog("namespace a; table X { v:int; }", "y.fbs");
  CHECK(!bad.errors.empty());  // no root catalog
  auto nofield = types::parse_catalog(
      R"(namespace a; table F { path:string; } table Outputs { f:F (tsugumi: "file"); } root_type Outputs; attribute "tsugumi";)", "z.fbs");
  CHECK(!nofield.errors.empty());  // File messages need a string value
}

TEST(structural_compatibility) {
  std::string err;
  auto a = types::compile("namespace one; table M { x:int; s:string; } root_type M;", "one.M", &err);
  auto b = types::compile("namespace two; table M { x:int; s:string; } root_type M;", "two.M", &err);
  auto c2 = types::compile("namespace three; table M { x:long; s:string; } root_type M;", "three.M", &err);
  auto d = types::compile("namespace four; table M { s:string; x:int; } root_type M;", "four.M", &err);
  CHECK(a && b && c2 && d);
  CHECK(types::compatible(**a, **b));    // same structure, other namespace
  CHECK(!types::compatible(**a, **c2));  // field type differs
  CHECK(!types::compatible(**a, **d));   // field ids differ
}

TEST(merge_union_keeps_origin) {
  std::string err;
  auto n = types::compile("namespace one; table N { v:ulong; } root_type N;", "one.N", &err);
  auto m = types::merged_type({{"left", *n}, {"right", *n}}, &err);
  CHECK(m.has_value());
  std::string bytes;
  CHECK(types::json_to_flatbuffer(**m, R"({"value_type":"right","value":{"v":7}})", &bytes, &err));
  std::string back;
  CHECK(types::flatbuffer_to_json(**m, bytes, &back, &err));
  CHECK(back.find("right") != std::string::npos);
}

// ---- graph and upstream analysis -------------------------------------------------

TEST(types_flow_through_fork_and_branch) {
  Project p;
  p.code("a", {});
  p.flag("cond");
  p.builtin("fork", R"({"template":"fork"})");
  p.builtin("branch", R"({"template":"branch"})");
  p.code("b", {"x"});
  auto r = p.resolve(c("a", "a") + "," + c("f", "fork") + "," + c("cond", "cond") + "," + c("br", "branch") + "," + c("b", "b"),
                     e("a", "out", "f", "in") + "," + e("f", "out1", "br", "value") + "," + e("cond", "flag", "br", "condition") +
                         "," + e("br", "a", "b", "x"));
  const auto* b = r->find({"b"});
  CHECK(b && b->input("x") && b->input("x")->type);
  CHECK_EQ(b->input("x")->type->root_type, std::string("t.Number"));
}

TEST(upstream_linear_chain) {
  Project p;
  p.code("a", {});
  p.code("b", {"x"});
  p.code("c", {"x"}, "{ run = \"${inputs.upstream.\"a\".run}\"; }");
  auto r = p.resolve(c("a", "a") + "," + c("b", "b") + "," + c("c", "c"), e("a", "out", "b", "x") + "," + e("b", "out", "c", "x"));
  CHECK_EQ(guaranteed(*r, "c"), (std::set<std::string>{"a", "b"}));
  CHECK_EQ(nix_refs(*r, "c"), (std::set<std::string>{"a", "b"}));
  CHECK(codes(*r, "c").empty());
}

TEST(upstream_branch_sides_do_not_survive_any_merge) {
  // A -> branch -> (B | C) -> any merge -> D: D may use A, never B or C.
  Project p;
  p.code("a", {});
  p.flag("cond");
  p.builtin("branch", R"({"template":"branch"})");
  p.code("b", {"x"});
  p.code("c", {"x"});
  p.code("d", {"l?", "r?"}, "{ run = \"${inputs.upstream.\"b\".run}\"; }", "any");
  auto r = p.resolve(c("a", "a") + "," + c("cond", "cond") + "," + c("br", "branch") + "," + c("b", "b") + "," + c("c", "c") + "," + c("d", "d"),
                     e("a", "out", "br", "value") + "," + e("cond", "flag", "br", "condition") + "," + e("br", "a", "b", "x") + "," +
                         e("br", "b", "c", "x") + "," + e("b", "out", "d", "l") + "," + e("c", "out", "d", "r"));
  auto g = guaranteed(*r, "d");
  CHECK(g.count("a") && g.count("br"));
  CHECK(!g.count("b") && !g.count("c"));
  CHECK_EQ(nix_refs(*r, "d"), (std::set<std::string>{"a"}));
  CHECK(codes(*r, "d").count("nix_reference_not_guaranteed"));
  CHECK_EQ(guaranteed(*r, "b"), (std::set<std::string>{"a", "br", "cond"}));
}

TEST(upstream_all_join_unites_required_inputs) {
  Project p;
  p.code("a", {});
  p.code("b", {"x"});
  p.code("c", {"x"});
  p.code("d", {"l", "r", "o?"});
  p.code("z", {});
  auto r = p.resolve(c("a", "a") + "," + c("b", "b") + "," + c("c", "c") + "," + c("d", "d") + "," + c("z", "z"),
                     e("a", "out", "b", "x") + "," + e("a", "out", "c", "x") + "," + e("b", "out", "d", "l") + "," +
                         e("c", "out", "d", "r") + "," + e("z", "out", "d", "o"));
  // Required inputs l and r: their guarantees add up; the optional o adds nothing.
  CHECK_EQ(guaranteed(*r, "d"), (std::set<std::string>{"a", "b", "c"}));
}

TEST(upstream_same_port_from_two_paths_keeps_common_part) {
  Project p;
  p.code("a", {});
  p.code("b", {"x"});
  p.code("c", {"x"});
  p.code("d", {"x"});
  auto r = p.resolve(c("a", "a") + "," + c("b", "b") + "," + c("c", "c") + "," + c("d", "d"),
                     e("a", "out", "b", "x") + "," + e("a", "out", "c", "x") + "," + e("b", "out", "d", "x") + "," + e("c", "out", "d", "x"));
  CHECK_EQ(guaranteed(*r, "d"), (std::set<std::string>{"a"}));
}

TEST(upstream_feedback_loop_converges_without_self) {
  // a -> loop(any: init, again) -> body -> loop.again
  Project p;
  p.code("a", {});
  p.code("loop", {"init?", "again?"}, "{ run = \"true\"; }", "any");
  p.code("body", {"x"}, "{ run = \"${inputs.upstream.\"body\".run}\"; }");
  auto r = p.resolve(c("a", "a") + "," + c("loop", "loop") + "," + c("body", "body"),
                     e("a", "out", "loop", "init") + "," + e("loop", "out", "body", "x") + "," + e("body", "out", "loop", "again"));
  CHECK_EQ(guaranteed(*r, "loop"), (std::set<std::string>{"a"}));
  CHECK_EQ(guaranteed(*r, "body"), (std::set<std::string>{"a", "loop"}));
  CHECK(codes(*r, "body").count("nix_reference_self"));
}

TEST(upstream_through_nested_subgraph) {
  Project p;
  p.code("a", {});
  p.code("inner", {"x"});
  p.code("d", {"x"}, "{ run = \"${inputs.upstream.\"s/inner\".run}\"; }");
  p.external("s", {"inner"}, "", R"({"inputs":{"in":{"card":"inner","port":"x"}},"outputs":{"out":{"card":"inner","port":"out"}}})");
  auto r = p.resolve(
      c("a", "a") + "," + c("s", "s") + "," + c("d", "d"),
      e("a", "out", "s", "in") + "," + e("s", "out", "d", "x"));
  CHECK_EQ(guaranteed(*r, "d"), (std::set<std::string>{"a", "s/inner"}));
  CHECK(codes(*r, "d").empty());
}

TEST(unreachable_cycle_guarantees_nothing) {
  Project p;
  p.code("x", {"in"});
  p.code("y", {"in"});
  auto r = p.resolve(c("x", "x") + "," + c("y", "y"), e("x", "out", "y", "in") + "," + e("y", "out", "x", "in"));
  CHECK(guaranteed(*r, "x").empty() && guaranteed(*r, "y").empty());
}

// ---- run plans --------------------------------------------------------------------

TEST(run_plans_whole_partial_and_subgraph) {
  Project p;
  p.builtin("trigger", R"({"template":"trigger"})");
  p.code("a", {});
  p.code("inner", {"x"});
  p.code("d", {"x"});
  p.external("s", {"inner"}, "", R"({"inputs":{"in":{"card":"inner","port":"x"}},"outputs":{"out":{"card":"inner","port":"out"}}})");
  auto r = p.resolve(
      c("go", "trigger") + "," + c("a", "a") + "," + c("s", "s") + "," + c("d", "d"),
      e("a", "out", "s", "in") + "," + e("s", "out", "d", "x"));
  auto whole = Scheduler::plan(*r, {"go"}, true);
  CHECK_EQ(whole.targets.size(), size_t(4));  // go, a, s/inner, d (not the container)
  auto single = Scheduler::plan(*r, {"d"}, false);
  CHECK_EQ(single.targets.size(), size_t(1));
  auto sub = Scheduler::plan(*r, {"s"}, false);
  CHECK_EQ(sub.targets.size(), size_t(1));
  CHECK_EQ(sub.seeds.size(), size_t(1));
  CHECK(sub.seeds[0] == (CardPath{"s", "inner"}));
  auto sub_down = Scheduler::plan(*r, {"s"}, true);
  CHECK_EQ(sub_down.targets.size(), size_t(2));
}

// ---- history ------------------------------------------------------------------------

TEST(history_completion_never_hides_newer_progress) {
  auto dir = temp_dir();
  {
    History h(dir / "h.sqlite", "secret");
    InvocationSummaryData running;
    running.run_id = "r1";
    running.card = {"a"};
    running.invocation = 2;
    running.state = CardState::running;
    running.phase = Phase::execute;
    running.started_at = 10;
    InvocationRecordData done;
    done.summary.run_id = "r1";
    done.summary.card = {"a"};
    done.summary.invocation = 1;
    done.summary.state = CardState::failed;
    done.summary.failure = FailureData::make(FailureKind::worker_failed, "boom");
    done.summary.build_cached = true;
    h.write_sync([&](sql::Db& db) {
      RunSummaryData s;
      s.status.run_id = "r1";
      s.status.state = RunState::running;
      s.target = {"a"};
      History::insert_run(db, s, "op1");
      History::put_status(db, running);
      h.complete_invocation(db, done);  // the older invocation completes later
    });
    auto list = h.read([](sql::Db& db) { return History::invocations(db, "r1", {"a"}, 0, 10); });
    CHECK_EQ(list.size(), size_t(2));
    CHECK(list[0].state == CardState::failed && list[1].state == CardState::running);
    CHECK(list[0].build_cached && *list[0].build_cached);
    auto latest = h.read([](sql::Db& db) { return History::latest_invocation(db, "r1", {"a"}, true); });
    CHECK(latest && *latest == 2);
    auto failure = h.read([](sql::Db& db) { return History::latest_failure(db, "r1", {"a"}); });
    CHECK(failure && failure->kind == FailureKind::worker_failed);
    auto agg = h.read([](sql::Db& db) { return History::aggregate(db, "r1", {"a"}); });
    CHECK(agg.calls == 1 && agg.failed == 1);
  }
  stdfs::remove_all(dir);
}

TEST(history_fault_fails_writes) {
  auto dir = temp_dir();
  {
    History h(dir / "h.sqlite", "secret", "history_invocation");
    InvocationRecordData rec;
    rec.summary.run_id = "r";
    rec.summary.card = {"a"};
    rec.summary.invocation = 1;
    bool threw = false;
    try {
      h.write_sync([&](sql::Db& db) { h.complete_invocation(db, rec); });
    } catch (const Failed& f) {
      threw = f.failure.kind == FailureKind::persistence_failed;
    }
    CHECK(threw);
  }
  stdfs::remove_all(dir);
}

// ---- send queue ---------------------------------------------------------------------

namespace {

struct Sink : RequestSink {
  std::pair<std::string, bool> hello(const RequestIn&) override { return {"hello-reply", true}; }
  void dispatch(RequestIn) override {}
  void disconnected(ConnId) override {}
};

std::string hello_frame() {
  flatbuffers::FlatBufferBuilder b;
  auto cid = b.CreateString("00000000-0000-4000-8000-000000000001");
  auto h = Tsugumi::RPC::CreateSessionHelloRequest(b, 1, cid);
  auto req = Tsugumi::RPC::CreateRequest(b, Tsugumi::RPC::RequestPayload::SessionHelloRequest, h.Union());
  auto msg = Tsugumi::RPC::CreateMessage(b, 1, Tsugumi::RPC::Payload::Request, req.Union());
  b.Finish(msg, "TSG1");
  uint32_t n = b.GetSize();
  return std::string(reinterpret_cast<const char*>(&n), 4) + std::string(reinterpret_cast<const char*>(b.GetBufferPointer()), n);
}

Note log_note(int i) {
  Note n;
  n.kind = Note::Kind::Log;
  n.approx_bytes = 64;
  n.build = std::make_shared<const Note::Build>([i](flatbuffers::FlatBufferBuilder& b) {
    auto run = b.CreateString("run-" + std::to_string(i));
    Tsugumi::RPC::LogAvailableBuilder lb(b);
    lb.add_run_id(run);
    lb.add_end_offset(static_cast<uint64_t>(i));
    return std::make_pair(Tsugumi::RPC::NotificationPayload::LogAvailable, lb.Finish().Union());
  });
  return n;
}

int connect_unix(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
  for (int i = 0; i < 100; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

std::optional<std::string> read_frame(int fd) {
  uint32_t n;
  size_t got = 0;
  while (got < 4) {
    ssize_t r = ::read(fd, reinterpret_cast<char*>(&n) + got, 4 - got);
    if (r <= 0) return std::nullopt;
    got += static_cast<size_t>(r);
  }
  std::string s(n, '\0');
  got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, s.data() + got, n - got);
    if (r <= 0) return std::nullopt;
    got += static_cast<size_t>(r);
  }
  return s;
}

}  // namespace

TEST(send_queue_keeps_order_and_drops_only_slow_peers) {
  auto dir = temp_dir();
  Sink sink;
  ServerOptions o;
  o.unix_path = (dir / "s").string();
  o.limits.outbox_messages = 64;
  Server server(o, &sink);
  server.start();
  int fast = connect_unix(o.unix_path);
  int slow = connect_unix(o.unix_path);
  CHECK(fast >= 0 && slow >= 0);
  auto hf = hello_frame();
  CHECK(::write(fast, hf.data(), hf.size()) == static_cast<ssize_t>(hf.size()));
  CHECK(read_frame(fast) == std::optional<std::string>("hello-reply"));
  CHECK(::write(slow, hf.data(), hf.size()) == static_cast<ssize_t>(hf.size()));
  CHECK(read_frame(slow) == std::optional<std::string>("hello-reply"));
  server.subscribe(1, "sub-fast", "open-fast");
  server.subscribe(2, "sub-slow", "open-slow");
  CHECK(read_frame(fast) == std::optional<std::string>("open-fast"));
  // The fast peer reads everything in order with consecutive seq; the slow
  // peer never reads, overflows its queue and is disconnected alone.
  std::thread reader([&] {
    uint64_t expect = 2;
    for (int i = 0; i < 5000; ++i) {
      auto f = read_frame(fast);
      if (!f) {
        fail(__FILE__, __LINE__, "fast peer lost its connection");
        return;
      }
      const auto* m = Tsugumi::RPC::GetMessage(f->data());
      const auto* n = m->payload_as_Notification();
      if (!n || n->seq() != expect) {
        fail(__FILE__, __LINE__, "notification out of order");
        return;
      }
      ++expect;
    }
  });
  for (int i = 0; i < 5000; ++i) {
    server.broadcast(log_note(i));
    if (i % 50 == 49) std::this_thread::sleep_for(std::chrono::milliseconds(2));  // a steady producer
  }
  reader.join();
  bool dropped = false;
  char buf[65536];
  for (int i = 0; i < 2000 && !dropped; ++i) {
    ssize_t r = ::read(slow, buf, sizeof buf);
    if (r == 0) dropped = true;
    if (r < 0) break;
  }
  CHECK(dropped);
  ::close(fast);
  ::close(slow);
  server.stop();
  stdfs::remove_all(dir);
}

int main() {
  int failed_cases = 0;
  for (const auto& t : cases()) {
    int before = g_failures;
    t.fn();
    bool ok = g_failures == before;
    failed_cases += !ok;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", t.name);
  }
  std::printf("%zu tests, %d failed\n", cases().size(), failed_cases);
  return failed_cases ? 1 : 0;
}
