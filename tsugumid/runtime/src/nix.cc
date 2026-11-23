#include "nix.hh"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <regex>
#include <sstream>

#include "nix_eval.hh"
#include "nix_params.hh"
#include "service.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/process.hh"

#ifndef TSUGUMI_CARD_NIXPKGS_REV
#define TSUGUMI_CARD_NIXPKGS_REV ""
#endif
#ifndef TSUGUMI_CARD_NIXPKGS_NARHASH
#define TSUGUMI_CARD_NIXPKGS_NARHASH ""
#endif

namespace tsugumi {

namespace stdfs = std::filesystem;

namespace {

// The interpreter the kernel runs ELF executables of a Linux system with
// (binfmt_misc: a static qemu-user in the shared VM,
// boot.binfmt.emulatedSystems on NixOS), or "" when none is registered. Matched
// by the ELF header the entry's magic names, whatever the entry is called.
std::string binfmt_interpreter(const std::string& system) {
  std::string machine = system == "x86_64-linux"    ? "3e00"
                        : system == "aarch64-linux" ? "b700"
                                                    : "";
  const stdfs::path dir = "/proc/sys/fs/binfmt_misc";
  std::error_code ec;
  if (machine.empty() || !stdfs::is_directory(dir, ec)) return "";
  std::ifstream status(dir / "status");
  std::string state;
  if (!std::getline(status, state) || state != "enabled") return "";
  for (const auto& e : stdfs::directory_iterator(dir, ec)) {
    auto name = e.path().filename().string();
    if (name == "register" || name == "status") continue;
    std::ifstream f(e.path());
    std::string line, interpreter, magic;
    bool enabled = false;
    while (std::getline(f, line)) {
      if (line == "enabled")
        enabled = true;
      else if (line.starts_with("interpreter "))
        interpreter = line.substr(12);
      else if (line.starts_with("magic "))
        magic = line.substr(6);
    }
    // 7f 'E' 'L' 'F', 64-bit, little-endian, ..., e_machine at byte 18.
    if (enabled && !interpreter.empty() && magic.starts_with("7f454c4602") &&
        magic.size() >= 40 && magic.substr(36, 4) == machine)
      return interpreter;
  }
  return "";
}

// A Nix string literal.
std::string nix_quote(const std::string& s) {
  std::string q = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\' || c == '$') q += '\\';
    q += c;
  }
  return q + "\"";
}

// File name of a project GC root: the Card (readable, plus a hash against
// collisions) and the result.
std::string root_prefix(const CardPath& card) {
  std::string k = path_key(card);
  std::string out;
  for (char c : k)
    out += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'
               ? c
               : '_';
  return out + "-" + crypto::sha256_hex(k).substr(0, 8) + "--";
}

auto dep_json(const CardPath& target, const std::string& name) -> std::string {
  auto key = schema::string_value(path_key(target));
  auto port = schema::string_value(name);
  return schema::object_text({{"key", &key}, {"name", &port}});
}

}  // namespace

NixRunner::NixRunner(Service& svc, stdfs::path eval_dir)
    : svc_(svc), eval_dir_(std::move(eval_dir)) {
  std::error_code ec;
  stdfs::remove_all(eval_dir_, ec);
  stdfs::create_directories(eval_dir_);
  // Restricted evaluation compares resolved paths (/tmp is /private/tmp on
  // macOS).
  eval_dir_ = stdfs::canonical(eval_dir_);
  nix_dir_ = stdfs::canonical(svc_.options().nix_dir, ec);
  if (const auto& file = svc_.options().prepared_cards; !file.empty()) {
    auto bytes = fs::read_file(file);
    if (!bytes)
      throw std::runtime_error("cannot read prepared builds " + file.string());
    auto manifest = flat::decode<Tsugumi::State::PreparedCards>(*bytes);
    if (!manifest)
      throw std::runtime_error("invalid prepared builds " + file.string());
    prepared_.emplace();
    for (auto& card : manifest->cards) {
      auto key = card->card;
      prepared_->emplace(std::move(key), std::move(*card));
    }
  }
  if (ec) nix_dir_ = svc_.options().nix_dir;
}

auto NixRunner::prepared(const std::string& card, const std::string& key,
                         uint32_t max_implementations) const -> NixResult {
  const auto found = prepared_->find(card);
  if (found == prepared_->end())
    throw Failed(FailureKind::unsupported,
                 "Card " + card + " has no prepared build", {{"card", card}});
  const auto& p = found->second;
  if (!p.error.empty())
    throw Failed(FailureKind::nix_failed,
                 "Card " + card + " did not build ahead: " + p.error,
                 {{"card", card}});
  if (p.key != key)
    throw Failed(FailureKind::validation_failed,
                 "Card " + card +
                     " changed since its build was prepared: its definition, "
                     "contract, Nix parameters or upstream exports differ",
                 {{"card", card}});
  const auto implementations = p.implementations;
  try {
    enforce_implementation_limit(implementations, max_implementations);
  } catch (const NixImplementationLimit& e) {
    throw Failed(FailureKind::execution_limit, e.what(), {{"card", card}});
  }
  NixResult res;
  res.derivation.path = p.derivation;
  res.derivation.output = p.plan;
  res.derivation.system = p.system;
  auto plan_json = fs::read_file(res.derivation.output);
  if (!plan_json)
    throw Failed(FailureKind::nix_failed,
                 "the prepared plan of " + card +
                     " is missing: " + res.derivation.output,
                 {{"card", card}});
  res.plan_json = std::move(*plan_json);
  res.closure = p.closure;
  res.execution_list = p.execution_list;
  res.implementation_count = static_cast<uint32_t>(implementations);
  res.implementation_labels = p.implementation_labels;
  try {
    check_implementation_labels(res.implementation_labels, implementations);
  } catch (const std::exception& e) {
    throw Failed(FailureKind::nix_failed, e.what(), {{"card", card}});
  }
  res.built = false;
  return res;
}

NixRunner::~NixRunner() { evaluator_.reset(); }

FlakeInfo NixRunner::resolve_flake(const std::string& ref) {
  static const std::regex page(
      R"(^https://github\.com/([^/\s]+)/([^/\s]+?)(\.git)?/?$)");
  static const std::regex allowed(
      R"(^(github:|gitlab:|sourcehut:|git\+https://|https://)[^\s"\\$]+$)");
  std::smatch m;
  std::string flake = std::regex_match(ref, m, page)
                          ? "github:" + m[1].str() + "/" + m[2].str()
                          : ref;
  if (!std::regex_match(flake, allowed))
    throw Failed(FailureKind::validation_failed,
                 "not a flake reference Tsugumi reads: " + ref);
  try {
    return evaluator().flake(NixCall{}, flake);
  } catch (const std::runtime_error& e) {
    throw Failed(FailureKind::nix_failed, e.what());
  }
}

NixEvaluator& NixRunner::evaluator() {
  std::lock_guard<std::mutex> l(eval_mu_);
  if (!evaluator_) {
    auto entry = fs::read_file(nix_dir_ / "card-entry.nix");
    if (!entry)
      throw std::runtime_error("cannot read the Card evaluator in " +
                               nix_dir_.string());
    std::vector<std::string> allowed{nix_dir_.string()};
    std::error_code ec;
    const auto kit = stdfs::canonical(nix_dir_ / "kit", ec);
    if (!ec) allowed.push_back(kit.string());
    allowed.push_back(eval_dir_.string());
    evaluator_ =
        std::make_unique<NixEvaluator>(std::move(*entry), std::move(allowed));
  }
  return *evaluator_;
}

stdfs::path NixRunner::cache_dir() const {
  return svc_.store().state_dir() / "cache";
}

stdfs::path NixRunner::gcroots_dir() const {
  const auto& d = svc_.options().gcroots_dir;
  return d.empty() ? cache_dir() / "gcroots" : d;
}

// nixpkgs.json is read by the Card's Nix evaluation.
struct NixpkgsPin {
  schema::t::String<"url"> url;
  schema::t::String<"narHash"> nar_hash;
};

static auto nixpkgs_pin(const std::string& pkgs) -> NixpkgsPin {
  std::string url;
  std::string hash;
  if (pkgs == "tsugumi:default") {
    std::string rev = TSUGUMI_CARD_NIXPKGS_REV;
    hash = TSUGUMI_CARD_NIXPKGS_NARHASH;
    if (rev.empty() || hash.empty())
      throw Failed(FailureKind::nix_failed,
                   "this build of tsugumid has no default nixpkgs pin");
    url = "https://github.com/NixOS/nixpkgs/archive/" + rev + ".tar.gz";
  } else {
    static const std::regex re(
        R"(^(https://github\.com/NixOS/nixpkgs/archive/[0-9a-f]{40}\.tar\.gz)#(sha256-[A-Za-z0-9+/=]{44})$)");
    std::smatch m;
    if (!std::regex_match(pkgs, m, re))
      throw Failed(FailureKind::nix_failed,
                   "pkgsPath must be tsugumi:default or a pinned nixpkgs "
                   "archive URL with #sha256-<hash>");
    url = m[1];
    hash = m[2];
  }
  NixpkgsPin pin;
  *pin.url = url;
  *pin.nar_hash = hash;
  return pin;
}

// NIXPATH-CONTRACT: lane A replaces these adapters with evaluator RPCs and
// records.
auto NixRunner::lock_source(const std::string& url) -> LockedSource {
  throw Failed(FailureKind::nix_failed,
               "nixPath locker is unavailable for " + url);
}

auto NixRunner::params_declaration(const stdfs::path& def_dir,
                                   std::string_view text) -> std::string {
  auto origin = def_dir.generic_string();
  if (!origin.ends_with('/')) origin += '/';
  return evaluate_literal_params(origin, text);
}

auto NixRunner::resolved_names(const CardPath&) const
    -> std::optional<std::vector<std::string>> {
  return std::nullopt;
}

std::map<std::string, std::string> NixRunner::generate(const Node& node) {
  std::map<std::string, std::string> files;
  files["_inputs.fbs"] = types::card_catalog(node.inputs, true);
  std::vector<std::pair<std::string, std::string>> pf;
  for (const auto& p : node.settings.params)
    pf.emplace_back(p.name, param_fbs_type(p.type));
  files["_params.fbs"] = types::params_schema(pf);
  files["_outputs.fbs"] = types::card_catalog(node.outputs, false);
  return files;
}

// A folder of the evaluation area named after its contents: written once,
// then reused by every later Card and run with the same contents.
stdfs::path NixRunner::content_dir(
    const char* kind, const std::string& digest,
    const std::function<std::map<std::string, std::string>()>& files) {
  auto dir = eval_dir_ / kind / digest.substr(0, 32);
  std::lock_guard<std::mutex> l(dirs_mu_);
  if (written_.count(dir.string())) return dir;
  std::error_code ec;
  if (!stdfs::is_directory(dir, ec)) {
    auto tmp = dir;
    tmp += ".tmp";
    stdfs::remove_all(tmp, ec);
    for (const auto& [name, content] : files()) {
      auto p = tmp / name;
      stdfs::create_directories(p.parent_path());
      std::ofstream(p, std::ios::binary | std::ios::trunc) << content;
    }
    stdfs::create_directories(tmp);
    stdfs::rename(tmp, dir);
  }
  written_.insert(dir.string());
  return dir;
}

auto NixRunner::execution_systems() -> std::vector<std::string> {
  std::string native = native_system();
  std::vector<std::string> systems{native};
  if (native.ends_with("-linux"))
    for (const char* system : {"aarch64-linux", "x86_64-linux"})
      if (native != system && !binfmt_interpreter(system).empty())
        systems.emplace_back(system);
  return systems;
}

auto CardBuildPlan::implementation(size_t index) const -> CardBuild {
  auto build = common;
  build.package = implementations.at(index);
  build.implementation_index = static_cast<uint32_t>(index + 1);
  if (index < implementation_labels.size())
    build.implementation_label = implementation_labels[index];
  return build;
}

auto NixRunner::build(const BuildRequest& req) -> CardBuildPlan {
  return build_plan(req, false);
}

auto NixRunner::declared_views(const BuildRequest& req)
    -> std::vector<schema::CardView> {
  const SharedStoreDerivationTransport transport;
  auto view_request = req;
  view_request.target = {WorkerKind::native, native_system()};
  view_request.transport = &transport;
  // Exported projects get their declarations from the prepared plan.
  if (prepared_ && !req.node->settings.system.empty())
    view_request.target.system = req.node->settings.system;
  return build_plan(view_request, !prepared_).common.views;
}

auto NixRunner::build_plan(const BuildRequest& req, bool views_only)
    -> CardBuildPlan {
  const Node& node = *req.node;
  if (req.target.system.empty())
    throw Failed(FailureKind::unsupported,
                 "execution target system is unknown");
  // Definition sources from the confirmed snapshot. _card.json, README.md and
  // _comments.json are not part of it: Nix gets what it needs from them
  // through the generated files, and settings or descriptions never
  // re-evaluate.
  auto definition_dir = [&](const std::string& prefix) {
    crypto::Sha256 h;
    std::vector<std::string> paths;
    for (const auto& [path, entry] : req.doc->sources) {
      if (!path.starts_with(prefix)) continue;
      auto rel = path.substr(prefix.size());
      if (rel == "_card.json" || rel == "README.md" || rel == "_comments.json")
        continue;
      paths.push_back(path);
      h.update(rel);
      h.update(entry.hash);
    }
    return content_dir("defs", h.finish_hex(), [&] {
      std::map<std::string, std::string> m;
      for (const auto& p : paths)
        m[p.substr(prefix.size())] = svc_.store().read_source(*req.doc, p);
      return m;
    });
  };
  auto generated_dir = [&](const std::map<std::string, std::string>& files) {
    crypto::Sha256 h;
    for (const auto& [name, content] : files) {
      h.update(name);
      h.update(std::to_string(content.size()));
      h.update(content);
    }
    return content_dir("gen", h.finish_hex(), [&] { return files; });
  };
  // NIXPATH-CONTRACT: lane A replaces this adapter with per-port types and
  // locked sources.
  auto with_sources = [&](const Node& n) {
    std::map<std::string, std::string> generated;
    for (auto& [name, content] : generate(n))
      generated.emplace("schemas/" + name, std::move(content));
    generated["params-nix.json"] = n.build_params_json;
    generated["sources.json"] = "[]";
    return generated;
  };

  // Another system's binaries run through the kernel's binfmt_misc: without
  // an entry for it, Nix would refuse the build with a less helpful message.
  std::string emulator;
  if (!views_only && req.target.kind == WorkerKind::native &&
      req.target.system != native_system()) {
    emulator = binfmt_interpreter(req.target.system);
    if (emulator.empty())
      throw Failed(
          FailureKind::unsupported,
          "system " + req.target.system +
              " needs an emulator registered with the kernel (binfmt_misc), " +
              "and Nix's extra-platforms to include it: the shared VM on a Mac "
              "has qemu-x86_64; " +
              "on NixOS, set boot.binfmt.emulatedSystems",
          {{"system", req.target.system}});
  }
  auto files = with_sources(node);
  const auto pin = nixpkgs_pin("tsugumi:default");
  files["nixpkgs.json"] = schema::encode(pin);
  // Only wired, guaranteed producers can supply Nix exports.
  const auto& graph = *req.doc->resolution;
  std::set<size_t> allowed;
  auto visit = [&](auto&& walk, const Node& owner) -> void {
    for (const auto& [_, port] : owner.nix_ports)
      if (port.exports_node && allowed.insert(*port.exports_node).second)
        walk(walk, graph.nodes[*port.exports_node]);
  };
  visit(visit, node);
  const auto dependencies = [&](const Node& owner) {
    std::string json = "[";
    for (const auto& [name, port] : owner.nix_ports) {
      if (!port.exports_node) continue;
      if (json.size() > 1) json += ",";
      json += dep_json(graph.nodes[*port.exports_node].path, name);
    }
    return json + "]";
  };
  std::string cards_json = "[";
  for (const size_t index : allowed) {
    const Node& u = graph.nodes[index];
    auto ugen = schema::string_value(generated_dir(with_sources(u)).string());
    auto udef = schema::string_value(definition_dir(u.def_dir).string());
    auto k = schema::string_value(path_key(u.path));
    auto deps = schema::capture(dependencies(u));
    if (cards_json.size() > 1) cards_json += ",";
    cards_json += schema::object_text(
        {{"key", &k}, {"gen", &ugen}, {"def", &udef}, {"deps", &deps}});
  }
  cards_json += "]";
  const auto deps_json = dependencies(node);
  std::string upstream_json;
  {
    auto c = schema::capture(cards_json);
    auto d = schema::capture(deps_json);
    upstream_json = schema::object_text({{"cards", &c}, {"deps", &d}});
  }
  // The upstream references hold this evaluator's folder paths: a folder of
  // their own, so the generated contract (a build input) depends on contents
  // only and builds are shared by copies of a project.
  crypto::Sha256 uh;
  uh.update(upstream_json);
  auto upstream = content_dir("up", uh.finish_hex(), [&] {
    return std::map<std::string, std::string>{{"upstream.json", upstream_json}};
  });
  auto gen = generated_dir(files);
  auto def = definition_dir(node.def_dir);
  NixCall call;
  call.pin_url = *pin.url;
  call.pin_hash = *pin.nar_hash;
  call.system = req.target.system;
  call.jobs = req.jobs;
  call.cores = req.cores;
  call.max_implementations = req.max_implementations;
  call.views_only = views_only;
  call.definition_dir = def;
  const bool needs_worker = req.target.kind == WorkerKind::remote ||
                            req.target.system != native_system();
  call.args = "{ nixDir = /. + " + nix_quote(nix_dir_.string()) +
              "; gen = /. + " + nix_quote(gen.string()) + "; upstream = /. + " +
              nix_quote((upstream / "upstream.json").string()) +
              "; card = /. + " + nix_quote(def.string()) +
              (needs_worker ? "; needsWorker = true" : "") +
              (views_only ? "; viewsOnly = true; }" : "; }");
  call.cancel = req.cancel;
  const auto budget_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration transfer_time{};
  if (req.timeout_seconds)
    call.deadline = budget_start + std::chrono::seconds(*req.timeout_seconds);
  call.log = req.log;
  call.on_build = req.on_build;
  call.on_phase = [&](bool building) {
    if (req.on_phase) req.on_phase(building ? Phase::build : Phase::evaluate);
  };
  const std::string prefix =
      (views_only ? "views-" : "") + root_prefix(node.path);
  NixResult res;
  const auto key = call.evaluation_key();
  const auto key_text = std::get<0>(key) + "\n" + std::get<1>(key) + "\n" +
                        std::get<2>(key) + "\n" + std::get<3>(key);
  if (prepared_) {
    if (req.on_phase) req.on_phase(Phase::evaluate);
    res = prepared(path_key(node.path), key_text, call.max_implementations);
    if (req.on_plan) req.on_plan(res.implementation_count);
  } else {
    try {
      call.root_dir = gcroots_dir();
      call.root_prefix = prefix;
      // Paths in args name content-addressed sources, generated contracts/Nix
      // params, and the transitive upstream definitions and flake products.
      // The evaluator validates its settings epoch and observed filesystem
      // reads.
      const auto card = path_key(node.path);
      std::optional<NixResult> earlier;
      {
        std::lock_guard l(evaluations_mu_);
        std::erase_if(evaluations_, [&](const auto& item) {
          return !graph.index.contains(item.first);
        });
        auto found = evaluations_.find(card);
        if (!views_only && found != evaluations_.end() &&
            found->second.first == key)
          earlier = found->second.second;
      }
      if (call.on_phase) call.on_phase(false);
      if (earlier && evaluator().reuse(call, *earlier)) {
        res = std::move(*earlier);
        enforce_implementation_limit(res.implementation_count,
                                     call.max_implementations);
        res.built = false;
      } else {
        uint64_t count = 0;
        {
          std::lock_guard l(evaluations_mu_);
          if (!views_only) evaluations_.erase(card);
          count = ++full_evaluations_;
        }
        log::info("Card full evaluation " + std::to_string(count));
        res = evaluator().evaluate(call);
      }
      if (req.on_plan) req.on_plan(res.implementation_count);
      if (!req.transport)
        throw std::runtime_error(
            "execution target has no derivation transport");
      const auto transfer_start = std::chrono::steady_clock::now();
      const auto build_deadline = call.deadline;
      call.deadline.reset();
      evaluator().make_available(call, *req.transport, res.derivation);
      transfer_time = std::chrono::steady_clock::now() - transfer_start;
      if (build_deadline) call.deadline = *build_deadline + transfer_time;
      if (req.target.kind == WorkerKind::native)
        res = evaluator().realise_native(call, std::move(res));
      std::lock_guard l(evaluations_mu_);
      std::erase_if(evaluations_, [&](const auto& item) {
        return item.second.second.evaluation_epoch != res.evaluation_epoch;
      });
      if (!views_only && res.evaluation_epoch) {
        if (evaluations_.size() >= 256 && !evaluations_.contains(card))
          evaluations_.erase(evaluations_.begin());
        evaluations_[card] = {key, res};
      }
    } catch (const NixInterrupted& e) {
      if (e.timed_out)
        throw Failed(FailureKind::timeout, e.what(),
                     {{"timeout_seconds",
                       std::to_string(req.timeout_seconds.value_or(0))}});
      throw Failed(FailureKind::cancelled, e.what());
    } catch (const NixImplementationLimit& e) {
      throw Failed(FailureKind::execution_limit, e.what(),
                   {{"card", path_key(node.path)}});
    } catch (const std::exception& e) {
      throw Failed(
          FailureKind::nix_failed,
          "Card preparation for system " + call.system + ": " + e.what(),
          {{"card", path_key(node.path)}, {"system", call.system}});
    }
  }
  CardBuildPlan plan;
  plan.evaluation_key = key_text;
  plan.execution_list = res.execution_list;
  plan.implementation_labels = res.implementation_labels;
  auto& build = plan.common;
  build.preparation_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - budget_start - transfer_time)
          .count();
  build.derivation = std::move(res.derivation);
  build.environment.derivation = build.derivation.path;
  build.environment.manifest = build.derivation.output;
  build.environment.system = build.derivation.system;
  if (req.target.kind == WorkerKind::remote) {
    plan.implementations.resize(res.implementation_count);
    return plan;
  }
  schema::CardPlan product;
  if (auto error = schema::decode_card_plan(res.plan_json, product))
    throw Failed(FailureKind::nix_failed,
                 "the Card build plan is invalid: " + error->message);
  plan.implementations = std::move(product.runs->value());
  if (plan.implementations.size() != res.implementation_count)
    throw Failed(FailureKind::nix_failed,
                 "the Card build plan has a different implementation count");
  build.views = std::move(product.views->value());
  build.emulator = emulator;
  if (const auto* worker = schema::get(product.worker)) build.worker = *worker;
  if (!views_only && needs_worker && build.worker.empty())
    throw Failed(FailureKind::nix_failed,
                 "the Card build plan lacks its target worker");
  build.environment.paths = std::move(res.closure);
  build.key = build.environment.manifest;
  build.cached = !res.built;
  return plan;
}

CacheClearOutcome NixRunner::clear_cache() {
  CacheClearOutcome out;
  auto state = svc_.store().state_dir();
  auto remove = [&](const stdfs::path& p, const std::string& label) {
    std::error_code ec;
    if (!stdfs::exists(p, ec)) return;
    out.freed_bytes += fs::directory_size(p);
    stdfs::remove_all(p, ec);
    if (ec)
      out.retained.push_back(label);
    else
      out.deleted.push_back(label);
  };
  // This project's GC roots: its builds become garbage for the shared store's
  // own cleanup (an app setting), which is never run from here.
  {
    std::error_code ec;
    size_t n = 0;
    for (const auto& e : stdfs::directory_iterator(gcroots_dir(), ec)) {
      if (e.path().filename() == ".name") continue;
      if (stdfs::remove(e.path(), ec)) ++n;
    }
    if (n) out.deleted.push_back("build-roots");
  }
  remove(state / "cache", "build-cache");
  std::error_code ec;
  if (stdfs::is_directory(state / "runs", ec)) {
    // A folder going away meanwhile (a run's files deleted) ends the listing,
    // never the clearing.
    for (const auto& run : stdfs::directory_iterator(state / "runs", ec)) {
      remove(run.path() / "build",
             "runs/" + run.path().filename().string() + "/build");
      remove(run.path() / "defs",
             "runs/" + run.path().filename().string() + "/defs");
      if (stdfs::exists(run.path() / "artifacts", ec))
        out.retained.push_back("runs/" + run.path().filename().string() +
                               "/artifacts");
    }
  }
  out.retained.push_back("history");
  out.retained.push_back("document");
  return out;
}

}  // namespace tsugumi
