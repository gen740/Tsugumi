// Behaviour tests of the core without RPC: port types, graph resolution and
// the Nix upstream analysis, run plans, history state selection, the
// notification send queue and the typed JSON schemas.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "graph.hh"
#include "history.hh"
#include "message_json.hh"
#include "nix_refs.hh"
#include "scheduler.hh"
#include "schema.hh"
#include "server.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "types.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

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

// The platform's SHA-256 and SHA-1 against known vectors, whole and in parts.
TEST(hashes_match_known_vectors) {
  CHECK(crypto::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(crypto::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::string million(1000000, 'a');
  CHECK(crypto::sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  for (size_t part : {1u, 63u, 64u, 65u, 4097u}) {
    crypto::Sha256 h;
    for (size_t i = 0; i < million.size(); i += part) h.update(million.data() + i, std::min(part, million.size() - i));
    CHECK(h.finish_hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
  auto s1 = crypto::sha1("abc");
  CHECK(crypto::hex(s1.data(), s1.size()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

// A Card's outputs are read below the run folder without following links:
// a link anywhere below the root (the file, a folder on the way) fails the
// read instead of reading what it points to.
TEST(outputs_are_read_without_following_links) {
  namespace sfs = std::filesystem;
  auto root = sfs::temp_directory_path() / ("tsugumi-beneath-" + crypto::uuid4());
  sfs::create_directories(root / "out" / "dir" / "sub");
  auto secret = root.parent_path() / ("tsugumi-secret-" + crypto::uuid4());
  std::ofstream(secret) << "secret";
  std::ofstream(root / "out" / "dir" / "a.txt") << "a";
  std::ofstream(root / "out" / "dir" / "sub" / "b.txt") << "b";
  sfs::create_symlink(secret, root / "out" / "dir" / "link.txt");
  sfs::create_directory_symlink(secret.parent_path(), root / "out" / "up");
  CHECK(fs::read_beneath(root, "out/dir/a.txt") == "a");
  auto refused = [&](const char* rel) {
    try {
      fs::read_beneath(root, rel);
      return false;
    } catch (const fs::Error&) {
      return true;
    }
  };
  CHECK(refused("out/dir/link.txt"));
  CHECK(refused(("out/up/" + secret.filename().string()).c_str()));
  CHECK(refused("out/../../etc/hosts"));
  CHECK(refused("out/dir"));
  auto l = fs::list_beneath(root, "out/dir");
  CHECK((l.files == std::vector<std::string>{"a.txt", "sub/b.txt"}));
  CHECK((l.links == std::vector<std::string>{"link.txt"}));
  sfs::remove_all(root);
  sfs::remove(secret);
}

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

// ---- a message as JSON for the Cards that read JSON ------------------------------

// A parser as the worker has one: from the binary schema the core makes for a port.
static std::unique_ptr<flatbuffers::Parser> message_parser(const std::string& schema, const std::string& root) {
  std::string error;
  auto type = types::compile(schema, root, &error);
  if (!type) fail(__FILE__, __LINE__, error);
  flatbuffers::IDLOptions o;
  o.strict_json = true;
  o.natural_utf8 = true;
  o.output_default_scalars_in_json = true;
  auto p = std::make_unique<flatbuffers::Parser>(o);
  p->known_attributes_["tsugumi"] = true;
  CHECK(p->Deserialize(reinterpret_cast<const uint8_t*>((*type)->bfbs.data()), (*type)->bfbs.size()));
  CHECK(p->SetRootType(root.c_str()));
  return p;
}

TEST(message_json_writes_what_gentext_writes_except_for_floats) {
  auto p = message_parser(R"(
    namespace t;
    enum Color : byte { Red, Green = 3 }
    enum Flags : ubyte (bit_flags) { A, B, C }
    enum Wide : ulong { Low, High = 18446744073709551615 }
    struct Inner { a:[int:3]; c:Color; }
    struct Point { x:double; y:float; ok:bool; inner:Inner; many:[Inner:2]; }
    table Leaf { name:string; v:long = -7; }
    table Nested { n:int; s:string; }
    union Either { Leaf, Nested }
    table Message {
      flag:bool; on:bool = true; i8:byte = -5; u8:ubyte = 200; i16:short; u16:ushort = 65535; i32:int; u32:uint;
      i64:long = -9000000000000000000; u64:ulong = 18446744073709551615; f:float = 1.5; d:double = 2.25; zero:double;
      absent:int = null; set:int = null; absent_d:double = null; set_d:double = null;
      nan_d:double; inf_d:double; minus_inf:float; whole:double; thousands:double;
      color:Color = Green; other:Color; unnamed:Color; flags:Flags; no_flag:Flags; bad_flag:Flags; wide:Wide; dep:int (deprecated);
      text:string; empty_text:string; point:Point; leaf:Leaf; default_leaf:Leaf; missing:Leaf;
      either:Either; none:Either; eithers:[Either];
      doubles:[double]; floats:[float]; bytes:[ubyte]; bools:[bool]; colors:[Color]; flag_list:[Flags]; texts:[string];
      leaves:[Leaf]; points:[Point]; nothing:[int]; no_tables:[Leaf];
      nested:[ubyte] (nested_flatbuffer: "Nested"); flex:[ubyte] (flexbuffer);
    }
    root_type Message;
  )", "t.Message");
  const bool parsed = p->ParseJson(R"({
    "flag": true, "on": false, "i8": -128, "u32": 4294967295, "d": -2.5, "f": 0.25, "set": 0, "set_d": 0.5,
    "nan_d": nan, "inf_d": inf, "minus_inf": -inf, "whole": 3.0, "thousands": 100000.0,
    "other": "Red", "unnamed": 7, "flags": "A C", "no_flag": 0, "bad_flag": 128, "wide": "High",
    "text": "日本語 \"quoted\" \\ \n\t \u0001 /", "empty_text": "",
    "point": {"x": 0.5, "y": -0.125, "ok": true, "inner": {"a": [1, 2, 3], "c": "Green"},
              "many": [{"a": [4, 5, 6], "c": "Red"}, {"a": [7, 8, 9], "c": 7}]},
    "leaf": {"name": "葉", "v": 1}, "default_leaf": {},
    "either_type": "Nested", "either": {"n": 1, "s": "x"},
    "eithers_type": ["Leaf", "Nested"], "eithers": [{"name": "a"}, {"n": 2}],
    "doubles": [0.5, 1.0, -1000000.0, 0.001, -0.0], "floats": [0.5, 1.0, -0.125], "bytes": [0, 255], "bools": [true, false],
    "colors": ["Green", "Red", 9], "flag_list": ["B", "A B", 0], "texts": ["x", "", "語"],
    "leaves": [{"name": "a"}, {}], "points": [],
    "nothing": [], "no_tables": [],
    "nested": [1, 2], "flex": [3, 4]
  })");
  if (!parsed) fail(__FILE__, __LINE__, p->error_);
  const void* buffer = p->builder_.GetBufferPointer();
  for (const int indent_step : {2, -1}) {  // a Card's input, a Logger's one line
    p->opts.indent_step = indent_step;
    std::string theirs, ours;
    CHECK(!flatbuffers::GenText(*p, buffer, &theirs));
    CHECK(!message_json::gen_text(*p, buffer, &ours));
    if (ours != theirs) fail(__FILE__, __LINE__, "GenText:\n" + theirs + "\nmessage_json:\n" + ours);
  }
}

TEST(message_json_writes_floats_exactly) {
  auto p = message_parser("namespace t; table M { d:double; f:float; } root_type M;", "t.M");
  const auto written = [&](const std::string& json) {
    if (!p->ParseJson(json.c_str())) fail(__FILE__, __LINE__, p->error_);
    const std::string bytes(reinterpret_cast<const char*>(p->builder_.GetBufferPointer()), p->builder_.GetSize());
    std::string out;
    CHECK(!message_json::gen_text(*p, p->builder_.GetBufferPointer(), &out));
    if (!p->ParseJson(out.c_str())) fail(__FILE__, __LINE__, p->error_);
    // The same value when read back.
    if (bytes != std::string(reinterpret_cast<const char*>(p->builder_.GetBufferPointer()), p->builder_.GetSize()))
      fail(__FILE__, __LINE__, "not the same when read back: " + out);
    return out;
  };
  for (const auto& [in, out] : std::vector<std::pair<std::string, std::string>>{
           {"0.042169650342858", "0.042169650342858"},
           {"0.30000000000000004", "0.30000000000000004"},
           {"123456.78901234567", "123456.78901234567"},
           {"1e-20", "1e-20"},
           {"-1.5e-7", "-1.5e-07"},
           {"5e-324", "5e-324"},
           {"1.7976931348623157e308", "1.7976931348623157e+308"},
           {"1e16", "1e+16"},
           {"9999999999999998", "9999999999999998.0"},
           {"0.0001", "0.0001"},
           {"3", "3.0"}}) {
    const std::string json = written(R"({"d": )" + in + "}");
    if (json.find(R"("d": )" + out + ",") == std::string::npos) fail(__FILE__, __LINE__, in + " -> " + json);
  }
  for (const auto& [in, out] : std::vector<std::pair<std::string, std::string>>{
           {"0.1", "0.1"}, {"16777216", "16777216.0"}, {"1.17549435e-38", "1.1754944e-38"}, {"3.4028235e38", "3.4028235e+38"}, {"1e-7", "1e-07"}}) {
    const std::string json = written(R"({"f": )" + in + "}");
    if (json.find(R"("f": )" + out + "\n") == std::string::npos) fail(__FILE__, __LINE__, in + " -> " + json);
  }
}

TEST(what_a_logger_shows_is_exact_and_on_one_line) {
  std::string err;
  auto type = types::compile("namespace t; table M { d:double; tiny:double; f:float; many:[double]; } root_type M;", "t.M", &err);
  CHECK(type.has_value());
  std::string bytes, shown;
  CHECK(types::json_to_flatbuffer(**type, R"({"d": 0.042169650342858, "tiny": 1e-20, "f": 0.1, "many": [1e-20, 3]})", &bytes, &err));
  CHECK(types::flatbuffer_to_json(**type, bytes, &shown, &err));
  CHECK_EQ(shown, R"({"d": 0.042169650342858,"tiny": 1e-20,"f": 0.1,"many": [1e-20,3.0]})");
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

// The written references: not in comments, not in the text of strings (an
// interpolation is code again), not an attribute of something else.
TEST(nix_references_are_read_from_code_only) {
  using Refs = std::vector<std::pair<bool, std::string>>;
  CHECK(nix_references(R"NIX({ pkgs, inputs, params }: {
    # upstream.ghost in a comment
    /* local.ghost in a block comment */
    run = ''
      cp local.conf "$out"            # a shell script's text
      echo ${inputs.upstream."a/b".run} ${inputs.local.c.exports.x}
      echo ''${upstream.escaped} '''
    '';
    note = "upstream.d \" ${inputs.upstream.e} \${upstream.notInterpolated}";
    other = pkgs.upstream.f;
    plain = with inputs; upstream.g;
    spaced = inputs . upstream . "h";
    dynamic = inputs.upstream."${"i"}";
  })NIX") == (Refs{{false, "a/b"}, {true, "c"}, {false, "e"}, {false, "g"}, {false, "h"}}));
}

// `local` names a Card of the same level only (entry.nix offers no other).
TEST(nix_local_names_a_card_of_this_level) {
  Project p;
  p.code("a", {});
  p.code("b", {"x"}, "{ run = \"${inputs.local.a.run} ${inputs.local.\"a/deeper\".run}\"; }");
  p.code("c", {"x"}, "{ run = ''\n  cp local.conf $out # upstream.ghost\n''; }");
  auto r = p.resolve(c("a", "a") + "," + c("b", "b") + "," + c("c", "c"), e("a", "out", "b", "x") + "," + e("a", "out", "c", "x"));
  bool deeper = false;
  for (const auto& d : r->nodes[r->index.at("b")].diags)
    deeper |= d.code == "nix_reference_unknown" && d.message.find("local.a/deeper") != std::string::npos;
  CHECK(deeper);
  CHECK(!codes(*r, "c").count("nix_reference_unknown"));
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

// A reply message as the server sends it (it reads the request id of every
// reply it queues, so replies are real messages, never placeholder bytes).
std::string reply_message(uint64_t request_id) {
  flatbuffers::FlatBufferBuilder b;
  b.Finish(Tsugumi::RPC::CreateMessage(b, request_id), "TSG1");
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

struct Sink : RequestSink {
  std::pair<std::string, bool> hello(const RequestIn&) override { return {reply_message(1), true}; }
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
  CHECK(read_frame(fast) == std::optional<std::string>(reply_message(1)));
  CHECK(::write(slow, hf.data(), hf.size()) == static_cast<ssize_t>(hf.size()));
  CHECK(read_frame(slow) == std::optional<std::string>(reply_message(1)));
  server.subscribe(1, "sub-fast", reply_message(2));
  server.subscribe(2, "sub-slow", reply_message(2));
  CHECK(read_frame(fast) == std::optional<std::string>(reply_message(2)));
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

// ---- typed JSON schemas (hosq) ---------------------------------------------------

namespace {
template <class S>
bool decodes(std::string_view text) {
  S s;
  return !schema::decode(text, s);
}
}  // namespace

TEST(schema_keys_that_share_a_prefix_select_their_own_member) {
  // "boundary" is a prefix of "boundaries", in either order.
  for (const char* text : {R"({"boundaries":{"a":{"inputs":{"x":1,"y":2}}},"boundary":{"outputs":{"x":3,"y":4}}})",
                           R"({"boundary":{"outputs":{"x":3,"y":4}},"boundaries":{"a":{"inputs":{"x":1,"y":2}}}})"}) {
    schema::Layout l;
    CHECK(!schema::decode(text, l));
    const auto* bs = schema::get(l.boundaries);
    const auto* b = schema::get(l.boundary);
    CHECK(bs && bs->entries().size() == 1 && bs->entries()[0].first == "a");
    CHECK(b && !schema::get(b->inputs) && schema::get(b->outputs));
  }
  // "tag" is a prefix of "tagMessage"; "time" shares "t".
  schema::HistoryCommit c;
  CHECK(!schema::decode(R"({"tagMessage":"m","time":7,"tag":"v1"})", c));
  CHECK(schema::get(c.tag) && *schema::get(c.tag) == "v1");
  CHECK(schema::get(c.tag_message) && *schema::get(c.tag_message) == "m");
  CHECK(schema::get(c.time) && *schema::get(c.time) == 7);
  // "rev", "receipt" and "execution_rev" in a schema whose members are all required.
  schema::JournalRecord r;
  CHECK(!schema::decode(
      R"({"t":"txn","rev":1,"execution_rev":2,"op":"o","kind":"k","digest":"d","receipt":"r","files":[]})", r));
  CHECK(*r.rev == 1 && *r.execution_rev == 2 && *r.receipt_b64 == "r");
}

TEST(schema_rejects_keys_near_a_member_name) {
  // Each value fits the member the key resembles, so only the key can fail.
  using schema::Layout;
  auto member = [](const std::string& key, const char* value) { return "{\"" + key + "\":" + value + "}"; };
  CHECK(decodes<Layout>(member("boundary", "{}")) && decodes<Layout>(member("boundaries", "{}")));
  CHECK(decodes<Layout>(member("flow", "\"leftToRight\"")));
  for (const char* key : {"boundar", "boundaryX", "boundariesX", "Boundary", "boundarie", "boundary\\u0000", ""})
    CHECK(!decodes<Layout>(member(key, "{}")));
  for (const char* key : {"flo", "flows", "Flow", " flow", "flow ", "fl\u00f6w", "\u30d5\u30ed\u30fc", "fl\\u006fw"})
    CHECK(!decodes<Layout>(member(key, "\"leftToRight\"")));  // an escaped spelling is not the member either
  // A required-member schema rejects an extra key that extends a member name.
  CHECK(!decodes<schema::JournalRecord>(
      R"({"t":"txn","rev":1,"revs":1,"execution_rev":2,"op":"o","kind":"k","digest":"d","receipt":"r","files":[]})"));
  // ... and a key that extends the name of a missing required member.
  CHECK(!decodes<schema::BuildCache>(R"({"outs":"x"})"));
}

TEST(schema_validation_rules) {
  using schema::CardDefinition;
  CHECK(!decodes<CardDefinition>(R"({"sandbox":true,"sandbox":false})"));                // duplicate key
  CHECK(!decodes<CardDefinition>(R"({"params":[{"name":"a","data":{"k":1,"k":2}}]})"));  // nested duplicate
  CHECK(!decodes<CardDefinition>("{\"description\":\"\xff\"}"));                          // invalid UTF-8
  CHECK(!decodes<CardDefinition>("{\"description\":\"\xc3\"}"));                          // truncated sequence
  CHECK(decodes<CardDefinition>("{\"description\":\"\xc3\xa9\"}"));
  CHECK(!decodes<CardDefinition>(R"({"timeoutSeconds":NaN})"));      // not JSON
  CHECK(!decodes<CardDefinition>(R"({"timeoutSeconds":Infinity})"));
  CHECK(!decodes<schema::Position>(R"({"x":1e999,"y":0})"));          // not finite as a number
  CHECK(!decodes<CardDefinition>(R"({"sandbox":"true"})"));        // type
  CHECK(!decodes<CardDefinition>(R"({"maxRuns":-1})"));
  CHECK(!decodes<CardDefinition>(R"({"maxRuns":1.5})"));
  CHECK(!decodes<CardDefinition>(R"({"maxRuns":18446744073709551616})"));
  CHECK(!decodes<CardDefinition>(R"({"sandbox":true} )" "x"));     // trailing text
  // An optional member set to null is not "absent": its type still applies.
  CHECK(!decodes<CardDefinition>(R"({"sandbox":null})"));
  // A captured value may be null; the core reads it as unset.
  CardDefinition d;
  CHECK(!schema::decode(R"({"timeoutSeconds":null})", d));
  CHECK(schema::get(d.timeout_seconds) && schema::is_null(*schema::get(d.timeout_seconds)));
}

TEST(schema_keeps_64_bit_integers_and_spellings) {
  schema::CardDefinition d;
  CHECK(!schema::decode(R"({"maxRuns":18446744073709551615,"params":[{"name":"p","data":1.50e+2}]})", d));
  CHECK(schema::get(d.max_runs) && *schema::get(d.max_runs) == std::numeric_limits<uint64_t>::max());
  const auto& param = (*schema::get(d.params))[0];
  CHECK_EQ(schema::text(*schema::get(param.data)), std::string("1.50e+2"));  // original spelling
  auto big = schema::capture("-9223372036854775808");
  CHECK_EQ(schema::text(big), std::string("-9223372036854775808"));
  // Encode and decode again: every value comes back unchanged.
  schema::JournalRecord r;
  CHECK(!schema::decode(R"({"t":"commit","rev":18446744073709551615,"execution_rev":0,"op":"é\n\"",)"
                        R"("kind":"k","digest":"","receipt":"","files":[{"p":"a/b","delete":false,"c":""}]})",
                        r));
  schema::JournalRecord again;
  CHECK(!schema::decode(schema::encode(r), again));
  CHECK(*again.rev == std::numeric_limits<uint64_t>::max());
  CHECK(*again.execution_rev == 0);
  CHECK_EQ(*again.op, std::string("\xc3\xa9\n\""));
  CHECK(again.files->size() == 1 && *(*again.files)[0].path == "a/b");
  CHECK_EQ(schema::encode(again), schema::encode(r));
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
