#include "agent/version_find.hh"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "host/failure.hh"
#include "tsugumi/document/document.hh"

namespace tsugumi::agent {
namespace {
using host::Failure;

// The core's graph limit is larger; 500 versions is several months of saves.
constexpr uint32_t kScannedVersions = 500;
constexpr uint64_t kDefaultCandidates = 10;
constexpr uint64_t kMaxCandidates = 30;
// Each record read is one run_record; the rest is left to the Agent.
constexpr size_t kRecordReads = 40;

auto Strings(const std::string& raw) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& value : json::Array(raw)) out.push_back(json::String(value));
  return out;
}

auto Quoted(const std::vector<std::string>& values) -> std::string {
  std::vector<std::string> out;
  for (const auto& value : values) out.push_back(json::Quote(value));
  return json::Encode(out);
}

auto CardPath(const std::string& key) -> std::string {
  std::vector<std::string> segments;
  for (size_t start = 0; start <= key.size();) {
    const auto end = std::min(key.find('/', start), key.size());
    const auto segment = std::string_view(key).substr(start, end - start);
    if (!valid_name(segment))
      throw Failure("invalid_request", "Invalid Card path: " + key);
    segments.push_back(json::Quote(segment));
    start = end + 1;
  }
  return json::Encode(json::Members{{"segments", json::Encode(segments)}});
}

// A Card in Card counts as changed when something inside it changed.
auto Covers(const std::string& card, const std::string& key) -> bool {
  return key == card || key.starts_with(card + "/");
}

auto ContentHash(const json::Members& value) -> std::string {
  return json::Text(value, "content_hash");
}

struct Output {
  std::string kind;
  std::string type;
  std::string hash;
};

struct Observation {
  bool recorded = false;
  std::string json;  // what the result reports for this record
  std::map<std::string, Output> outputs;
};

class Finder {
 public:
  Finder(const ProjectRead& read, std::string observe)
      : read_(read), observe_(std::move(observe)) {}

  auto Observe(const std::string& run) -> const Observation& {
    if (const auto found = seen_.find(run); found != seen_.end())
      return found->second;
    Observation observation;
    json::Members out{{"card", json::Quote(observe_)}};
    auto unverified = [&](const std::string& why) {
      out["unverified"] = json::Quote(why);
      observation.json = json::Encode(out);
      return observation;
    };
    if (reads_ >= kRecordReads) {
      // Not cached: the budget is per call, not a fact about the run.
      budget_ = unverified(
          "Not read: this call's record reads are spent; read it with "
          "run_record");
      return budget_;
    }
    ++reads_;
    json::Members record;
    try {
      record = read_("run_record", {{"run_id", json::Quote(run)},
                                    {"card", CardPath(observe_)},
                                    {"follow_latest", "true"}});
    } catch (const Failure& failure) {
      return seen_[run] = unverified(
                 failure.code == "not_found"
                     ? "The run's record is not kept (deleted or never "
                       "recorded)"
                     : failure.code + ": " + failure.what());
    }
    if (!json::Bool(json::Get(record, "exists"), true))
      return seen_[run] = unverified("This Card did not run in this run");
    if (!record.contains("record"))
      return seen_[run] = unverified("No invocation record is kept");
    const auto invocation = json::Object(record.at("record"));
    const auto summary = json::Object(json::Get(invocation, "summary", "{}"));
    const auto number = json::String(json::Get(summary, "invocation"),
                                     json::Get(summary, "invocation"));
    out["invocation"] = json::Quote(number);
    out["state"] = json::Get(summary, "state", "null");
    out["link"] = json::Quote("run:" + run + "/" + observe_ + "#" + number);
    std::vector<std::string> notes;
    if (json::Bool(json::Get(record, "recording_disabled")))
      notes.push_back("Outputs are not recorded for this Card");
    std::vector<std::string> outputs;
    for (const auto& raw : json::Array(json::Get(invocation, "outputs", "[]"))) {
      const auto value = json::Object(raw);
      const Output output{json::Text(value, "kind"),
                          json::Text(value, "root_type"), ContentHash(value)};
      const auto port = json::Text(value, "name");
      observation.outputs[port] = output;
      json::Members row{{"port", json::Quote(port)},
                        {"type", json::Quote(output.type)}};
      if (!output.kind.empty()) row["kind"] = json::Quote(output.kind);
      row["hash"] = output.hash.empty() ? "null" : json::Quote(output.hash);
      outputs.push_back(json::Encode(row));
    }
    out["outputs"] = json::Encode(outputs);
    if (!notes.empty()) out["unverified"] = Quoted(notes);
    observation.recorded = true;
    observation.json = json::Encode(out);
    return seen_[run] = observation;
  }

  // Same, changed or unknown for each output port of `after` against `before`.
  static auto Compare(const Observation& before, const Observation& after)
      -> std::string {
    std::set<std::string> ports;
    for (const auto& [port, _] : before.outputs) ports.insert(port);
    for (const auto& [port, _] : after.outputs) ports.insert(port);
    std::vector<std::string> rows;
    for (const auto& port : ports) {
      json::Members row{{"port", json::Quote(port)}};
      const auto a = before.outputs.find(port), b = after.outputs.find(port);
      std::string result = "changed", why;
      if (a == before.outputs.end())
        why = "absent before";
      else if (b == after.outputs.end())
        why = "absent after";
      else if (a->second.kind != b->second.kind ||
               a->second.type != b->second.type)
        why = "kind or type differs";
      else if (a->second.hash.empty() || b->second.hash.empty())
        result = "unknown", why = "no recorded content hash";
      else if (a->second.hash == b->second.hash)
        result = "same";
      row["result"] = json::Quote(result);
      if (!why.empty()) row["why"] = json::Quote(why);
      rows.push_back(json::Encode(row));
    }
    return json::Encode(rows);
  }

 private:
  const ProjectRead& read_;
  std::string observe_;
  std::map<std::string, Observation> seen_;
  Observation budget_;
  size_t reads_ = 0;
};

auto RunsNewestFirst(const json::Members& commit) -> std::vector<json::Members> {
  std::vector<json::Members> runs;
  for (const auto& raw : json::Array(json::Get(commit, "runs", "[]")))
    runs.push_back(json::Object(raw));
  // `ended` is ISO 8601 UTC, so text order is time order.
  std::stable_sort(runs.begin(), runs.end(), [](const auto& a, const auto& b) {
    return json::Text(a, "ended") > json::Text(b, "ended");
  });
  return runs;
}
}  // namespace

auto FindVersions(const json::Members& args, const ProjectRead& read)
    -> std::string {
  const auto card = json::Text(args, "card");
  if (card.empty()) throw Failure("invalid_request", "card is required");
  const auto observe = json::Text(args, "observe", card);
  (void)CardPath(card);
  (void)CardPath(observe);
  std::set<std::string> kinds;
  if (args.contains("kinds"))
    for (const auto& kind : Strings(args.at("kinds"))) kinds.insert(kind);
  const auto wanted = std::clamp<uint64_t>(
      json::UInt(json::Get(args, "limit"), kDefaultCandidates), 1,
      kMaxCandidates);

  const auto graph = read(
      "version_graph", {{"limit", std::to_string(kScannedVersions)}});
  json::Members result{{"card", json::Quote(card)},
                       {"card_link", json::Quote("card:" + card)},
                       {"observe", json::Quote(observe)}};
  std::vector<std::string> unverified;
  if (!json::Bool(json::Get(graph, "enabled"))) {
    result["candidates"] = "[]";
    result["unverified"] = Quoted(
        {"The project has no version history, so changes are not recorded"});
    return json::Encode(result);
  }
  std::vector<json::Members> commits;
  std::map<std::string, size_t> index;
  for (const auto& raw : json::Array(json::Get(graph, "commits", "[]"))) {
    auto commit = json::Object(raw);
    index[json::Text(commit, "id")] = commits.size();
    commits.push_back(std::move(commit));
  }
  result["scanned_versions"] = std::to_string(commits.size());
  if (commits.size() >= kScannedVersions)
    unverified.push_back("Only the newest " + std::to_string(kScannedVersions) +
                         " versions were scanned");

  Finder finder(read, observe);
  // The nearest first-parent ancestor's newest run that recorded `observe`:
  // the trial before the change.
  auto before = [&](const json::Members& commit)
      -> std::optional<std::pair<std::string, std::string>> {
    auto parents = Strings(json::Get(commit, "parents", "[]"));
    while (!parents.empty()) {
      const auto found = index.find(parents.front());
      if (found == index.end()) return std::nullopt;
      const auto& parent = commits[found->second];
      for (const auto& run : RunsNewestFirst(parent)) {
        const auto id = json::Text(run, "run_id");
        if (finder.Observe(id).recorded)
          return std::pair{json::Text(parent, "id"), id};
      }
      parents = Strings(json::Get(parent, "parents", "[]"));
    }
    return std::nullopt;
  };

  std::vector<std::string> candidates;
  bool more = false;
  for (const auto& commit : commits) {
    bool touched = false;
    for (const auto& key : Strings(json::Get(commit, "changed", "[]")))
      touched = touched || Covers(card, key);
    if (!touched) continue;
    if (candidates.size() == wanted) {
      more = true;
      break;
    }
    const auto id = json::Text(commit, "id");
    json::Members candidate{{"commit", json::Quote(id)}};
    for (const auto* field : {"message", "summary", "time", "tags"})
      if (commit.contains(field)) candidate[field] = commit.at(field);
    std::vector<std::string> notes;
    try {
      const auto note = read("version_note", {{"commit", json::Quote(id)}});
      const auto changes = json::Object(json::Get(note, "changes", "{}"));
      std::set<std::string> found;
      for (const auto& key : Strings(json::Get(changes, "added", "[]")))
        if (Covers(card, key)) found.insert("added");
      for (const auto& key : Strings(json::Get(changes, "removed", "[]")))
        if (Covers(card, key)) found.insert("removed");
      for (const auto& raw : json::Array(json::Get(changes, "changed", "[]"))) {
        const auto change = json::Object(raw);
        if (!Covers(card, json::Text(change, "card"))) continue;
        for (const auto& kind : Strings(json::Get(change, "kinds", "[]")))
          found.insert(kind);
      }
      if (!kinds.empty() &&
          std::none_of(found.begin(), found.end(),
                       [&](const auto& kind) { return kinds.contains(kind); }))
        continue;
      candidate["changes"] = Quoted({found.begin(), found.end()});
    } catch (const Failure& failure) {
      // Only files that are not part of the run definition changed, or the
      // record could not be read: the kinds are not known.
      if (!kinds.empty()) continue;
      notes.push_back("How it changed is not known: " +
                      std::string(failure.what()));
    }
    const auto earlier = before(commit);
    const Observation* baseline = nullptr;
    if (earlier) {
      baseline = &finder.Observe(earlier->second);
      candidate["before"] = json::Encode(json::Members{
          {"commit", json::Quote(earlier->first)},
          {"run_id", json::Quote(earlier->second)},
          {"link", json::Quote("run:" + earlier->second)},
          {"observed", baseline->json}});
    } else {
      notes.push_back(
          "No earlier version's run recorded the observed Card, so there is "
          "nothing to compare against");
    }
    std::vector<std::string> runs;
    for (const auto& run : RunsNewestFirst(commit)) {
      const auto run_id = json::Text(run, "run_id");
      json::Members row{{"run_id", json::Quote(run_id)},
                        {"link", json::Quote("run:" + run_id)}};
      for (const auto* field : {"state", "ended"})
        if (run.contains(field)) row[field] = run.at(field);
      const auto& observed = finder.Observe(run_id);
      auto observation = json::Object(observed.json);
      if (baseline && baseline->recorded && observed.recorded)
        observation["compared"] = Finder::Compare(*baseline, observed);
      row["observed"] = json::Encode(observation);
      runs.push_back(json::Encode(row));
    }
    if (runs.empty()) notes.push_back("No run of this version is recorded");
    candidate["runs"] = json::Encode(runs);
    if (!notes.empty()) candidate["unverified"] = Quoted(notes);
    candidates.push_back(json::Encode(candidate));
  }
  result["candidates"] = json::Encode(candidates);
  if (more) result["more_candidates"] = "true";
  if (!unverified.empty()) result["unverified"] = Quoted(unverified);
  return json::Encode(result);
}

}  // namespace tsugumi::agent
