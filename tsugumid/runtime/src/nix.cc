#include "nix.hh"

#include <sys/stat.h>
#include <unistd.h>

#include <charconv>
#include <fstream>
#include <sstream>

#include "nix_eval.hh"
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

auto project_pins(const Document& doc, const Store& store, const Node& node)
    -> std::pair<LockedNixPath, std::string> {
  const auto path = node.project_dir + "tsugumi.lock";
  std::map<std::string, json::Members> lines;
  if (doc.sources.contains(path)) {
    const auto lock = json::Object(store.read_source(doc, path), path);
    for (const auto& row : json::Array(json::Get(lock, "nixPath", "[]"))) {
      auto entry = json::Object(row, path);
      const auto name = json::Text(entry, "name");
      if (!lines.emplace(name, std::move(entry)).second)
        throw std::runtime_error(path + ": duplicate nixPath name " +
                                 json::Quote(name));
    }
  }

  LockedNixPath result;
  json::Members identity;
  for (const auto& declaration : node.nix_path) {
    const auto& name = declaration.name;
    const auto line = lines.find(name);
    const auto prefix = "nixPath " + json::Quote(name);
    if (line == lines.end())
      throw std::runtime_error(prefix + " is not locked; update the lock");
    const auto url = json::Text(line->second, "url");
    if (declaration.url != url)
      throw std::runtime_error(prefix + " changed (lock has " + url +
                               "); update the lock");
    const auto source = json::Object(json::Get(line->second, "locked"), path);
    const auto timestamp = json::Get(source, "lastModified", "0");
    int64_t last_modified = 0;
    const auto [end, err] = std::from_chars(
        timestamp.data(), timestamp.data() + timestamp.size(), last_modified);
    if (err != std::errc{} || end != timestamp.data() + timestamp.size() ||
        last_modified < 0)
      throw std::runtime_error(path + ": invalid lastModified for " + prefix);
    result.emplace(
        name,
        LockedSource{json::Text(source, "type"), json::Text(source, "owner"),
                     json::Text(source, "repo"), json::Text(source, "rev"),
                     json::Text(source, "narHash"), last_modified});
    identity.emplace(name, json::Encode(line->second));
  }
  return {std::move(result), json::Encode(identity)};
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

auto NixRunner::lock_source(const std::string& url) -> LockedSource {
  return evaluator().lock_source(url);
}

auto NixRunner::params_declaration(const stdfs::path& def_dir) -> std::string {
  auto& store = svc_.store();
  auto path = def_dir.is_absolute() ? def_dir.lexically_relative(store.root())
                                    : def_dir;
  path /= "params.nix";
  if (!fs::valid_relative_path(path.generic_string()))
    throw std::runtime_error("invalid params.nix path");
  const auto doc = store.current();
  if (!doc->sources.contains(path.generic_string()))
    return R"({"run":[],"build":[]})";
  return params_declaration(def_dir,
                            store.read_source(*doc, path.generic_string()));
}

auto NixRunner::params_declaration(const stdfs::path& def_dir,
                                   std::string_view text) -> std::string {
  NixCall call;
  call.system = native_system();
  return evaluator().params_declaration(
      call, text, (def_dir / "params.nix").generic_string());
}

auto NixRunner::resolved_names(const CardPath& card) const
    -> std::optional<std::vector<std::string>> {
  std::lock_guard lock(names_mu_);
  const auto found = resolved_names_.find(path_key(card));
  if (found == resolved_names_.end()) return std::nullopt;
  return found->second.second;
}

auto NixRunner::remember_names(const ResolvedNames& names, uint64_t order)
    -> void {
  std::lock_guard lock(names_mu_);
  for (const auto& [card, observed] : names) {
    auto& record = resolved_names_[card];
    if (order > record.first) record = {order, observed};
  }
}

NixEvaluator& NixRunner::evaluator() {
  std::lock_guard<std::mutex> l(eval_mu_);
  if (!evaluator_) {
    auto entry = fs::read_file(nix_dir_ / "card-entry.nix");
    if (!entry)
      throw std::runtime_error("cannot read the Card evaluator in " +
                               nix_dir_.string());
    evaluator_ = std::make_unique<NixEvaluator>(std::move(*entry));
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

// A folder of the evaluation area named after its contents: written once,
// then reused by every later Card and run with the same contents.
stdfs::path NixRunner::content_dir(
    const char* kind, const std::string& digest,
    const std::function<std::map<std::string, std::string>()>& files) {
  auto dir = eval_dir_ / kind / digest;
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
      std::ofstream output(p, std::ios::binary | std::ios::trunc);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output.write(content.data(), content.size());
      output.close();
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

auto NixRunner::configure_call(NixCall& call, const Document& doc,
                               const Node& node, bool expression_only) -> void {
  const auto& graph = *doc.resolution;
  std::set<size_t> upstream;
  auto visit = [&](auto&& walk, const Node& owner) -> void {
    for (const auto& [_, port] : owner.nix_ports)
      if (port.exports_node && upstream.insert(*port.exports_node).second)
        walk(walk, graph.nodes.at(*port.exports_node));
  };
  visit(visit, node);

  std::map<std::string, std::string> project_keys;
  const auto add_project = [&](const Node& owner) {
    if (project_keys.contains(owner.project_dir)) return;
    auto [pins, key] = project_pins(doc, svc_.store(), owner);
    project_keys.emplace(owner.project_dir, std::move(key));
    call.projects.emplace(owner.project_dir, std::move(pins));
  };
  add_project(node);
  for (auto index : upstream) add_project(graph.nodes.at(index));

  crypto::Sha256 key;
  const auto prepare = [&](const Node& owner) -> json::Members {
    const auto card = path_key(owner.path);
    const auto& project = project_keys.at(owner.project_dir);
    auto& store = svc_.store();
    crypto::Sha256 source_hash;
    source_hash.update(json::Quote(card));
    source_hash.update(json::Quote(owner.project_dir));
    source_hash.update(project);
    std::vector<std::string> paths;
    for (auto it = doc.sources.lower_bound(owner.def_dir);
         it != doc.sources.end() && it->first.starts_with(owner.def_dir);
         ++it) {
      const auto rel = it->first.substr(owner.def_dir.size());
      if (rel == "_card.json" || rel == "README.md" || rel == "_comments.json")
        continue;
      paths.push_back(it->first);
      source_hash.update(json::Quote(rel));
      source_hash.update(it->second.hash);
    }
    const auto digest = source_hash.finish_hex();
    const auto definition = content_dir("defs", digest, [&] {
      std::map<std::string, std::string> files;
      for (const auto& path : paths)
        files.emplace(path.substr(owner.def_dir.size()),
                      store.read_source(doc, path));
      return files;
    });

    std::vector<std::string> deps;
    std::map<std::string, std::string> files{
        {"build-params.json", owner.build_params_json}};
    for (const auto& [name, port] : owner.nix_ports) {
      if (!fs::valid_relative_path(name) || stdfs::path(name).has_parent_path())
        throw std::runtime_error("invalid input port name " +
                                 json::Quote(name));
      json::Members input{{"name", json::Quote(name)}};
      if (port.exports_node)
        input.emplace(
            "producer",
            json::Quote(path_key(graph.nodes.at(*port.exports_node).path)));
      deps.push_back(json::Encode(input));
      files.emplace("types/" + name + ".fbs",
                    types::card_catalog({port.type}, true));
    }
    crypto::Sha256 generated_hash;
    for (const auto& [name, content] : files) {
      generated_hash.update(json::Quote(name));
      generated_hash.update(crypto::sha256_hex(content));
    }
    const auto gen_key = generated_hash.finish_hex();
    const auto generated = content_dir("gen", gen_key, [&] { return files; });
    call.definitions.push_back({definition.string(), card, owner.project_dir});
    call.generated.push_back(generated.string());
    key.update(digest);
    key.update(gen_key);
    key.update(json::Encode(deps));
    return {{"key", json::Quote(card)},
            {"def", json::Quote(definition.string())},
            {"gen", json::Quote(generated.string())},
            {"deps", json::Encode(deps)}};
  };

  const auto self = prepare(node);
  std::vector<std::string> cards;
  for (const auto index : upstream)
    cards.push_back(json::Encode(prepare(graph.nodes.at(index))));
  const auto manifest = json::Encode(
      json::Members{{"cards", json::Encode(cards)}, {"deps", self.at("deps")}});
  const auto manifest_dir =
      content_dir("up", crypto::sha256_hex(manifest), [&] {
        return std::map<std::string, std::string>{{"upstream.json", manifest}};
      });
  call.generated.push_back(manifest_dir.string());
  call.definition_dir = json::Text(self, "def");
  call.card = path_key(node.path);
  call.args = "{ gen = /. + " + nix_quote(json::Text(self, "gen")) +
              "; upstream = /. + " +
              nix_quote((manifest_dir / "upstream.json").string()) +
              "; card = /. + " + nix_quote(call.definition_dir.string()) +
              "; cardKey = " + nix_quote(call.card) +
              (call.views_only ? "; viewsOnly = true" : "") +
              (expression_only ? "; expressionOnly = true" : "") + "; }";
  call.nix_dir = nix_dir_;
  call.core_nixpkgs = {"github",
                       "NixOS",
                       "nixpkgs",
                       TSUGUMI_CARD_NIXPKGS_REV,
                       TSUGUMI_CARD_NIXPKGS_NARHASH,
                       0};
  key.update(json::Quote(call.core_nixpkgs.rev));
  key.update(json::Quote(call.core_nixpkgs.nar_hash));
  key.update(call.needs_worker ? "worker" : "no-worker");
  key.update(call.views_only ? "views" : "plan");
  call.definition_key = key.finish_hex();
}

auto NixRunner::evaluate_expression(const ExpressionRequest& req)
    -> ExpressionResult {
  if (!req.doc || !req.doc->resolution)
    throw std::runtime_error("Card expression requires a resolved project");
  const auto* node = req.doc->resolution->find(req.card);
  if (!node || (node->kind != CardType::Code && node->kind != CardType::Draft))
    throw std::runtime_error("unknown definition for Card " +
                             path_key(req.card));
  if (req.expression.size() > kMaxDocumentJsonBytes)
    throw std::runtime_error("Nix expression exceeds 8 MiB");
  NixCall call;
  call.system = req.system.empty() ? native_system() : req.system;
  call.cancel = req.cancel;
  call.deadline = req.deadline;
  call.log = req.log;
  configure_call(call, *req.doc, *node, true);
  auto result = evaluator().expression(call, req.expression, req.build);
  remember_names(result.resolved_names, result.observation_order);
  return result;
}

auto NixRunner::build_plan(const BuildRequest& req, bool views_only)
    -> CardBuildPlan {
  const Node& node = *req.node;
  if (req.target.system.empty())
    throw Failed(FailureKind::unsupported,
                 "execution target system is unknown");
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
  NixCall call;
  call.system = req.target.system;
  call.jobs = req.jobs;
  call.cores = req.cores;
  call.max_implementations = req.max_implementations;
  call.views_only = views_only;
  const bool needs_worker = req.target.kind == WorkerKind::remote ||
                            req.target.system != native_system();
  call.needs_worker = needs_worker && !views_only;
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
  try {
    configure_call(call, *req.doc, node);
  } catch (const Failed&) {
    throw;
  } catch (const std::exception& error) {
    throw Failed(FailureKind::nix_failed, error.what(),
                 {{"card", path_key(node.path)}, {"system", call.system}});
  }
  const auto key = call.evaluation_key();
  const auto key_text = std::get<0>(key) + "\n" + std::get<1>(key);
  if (prepared_) {
    if (req.on_phase) req.on_phase(Phase::evaluate);
    res = prepared(path_key(node.path), key_text, call.max_implementations);
    if (req.on_plan) req.on_plan(res.implementation_count);
  } else {
    try {
      call.root_dir = gcroots_dir();
      call.root_prefix = prefix;
      // Paths in args name content-addressed sources, generated contracts/Nix
      // params, and the transitive upstream definitions and locks.
      // The evaluator validates its settings epoch and observed filesystem
      // reads.
      const auto card = std::pair{call.card, call.system};
      std::optional<NixResult> earlier;
      {
        std::lock_guard l(evaluations_mu_);
        std::erase_if(evaluations_, [&](const auto& item) {
          return !req.doc->resolution->index.contains(item.first.first);
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
      remember_names(res.resolved_names, res.observation_order);
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
