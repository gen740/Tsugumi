#include "host/version_text.hh"

#include <algorithm>

namespace tsugumi::host {
namespace {
auto Names(const std::vector<std::string>& names, bool en) -> std::string {
  std::string text;
  for (size_t i = 0; i < std::min(names.size(), size_t{6}); ++i)
    text += (i ? (en ? ", " : "、") : "") + names[i];
  if (names.size() > 6)
    text += en ? " and " + std::to_string(names.size() - 6) + " more"
               : " ほか " + std::to_string(names.size() - 6);
  return text;
}
}  // namespace

auto CommitText(const Tsugumi::RPC::VersionCommitFactsT& record, bool en)
    -> std::string {
  if (record.kind == "create")
    return en ? "Create the project" : "プロジェクトを作成";
  if (record.kind == "checkout")
    return en ? "Changes in progress" : "作業中の変更";
  if (record.kind == "run")
    return std::string(en ? "Run: " : "実行: ") +
           (record.card.empty() ? "Workflow" : record.card);
  return record.kind;
}
auto RunText(const Tsugumi::RPC::VersionRunFactsT& record, bool en)
    -> std::string {
  std::vector<std::string> ran, failed;
  for (const auto& count : record.ran)
    ran.push_back(count->card + (count->count > 1
                                     ? " ×" + std::to_string(count->count)
                                     : ""));
  for (const auto& failure : record.failures)
    failed.push_back(failure->card + (en ? " (" : "（") + failure->kind +
                     (en ? ")" : "）"));
  std::string out =
      en ? (record.whole ? "whole"
                         : (record.propagate ? "from " : "only ") + record.card)
         : (record.whole
                ? "全体"
                : record.card + (record.propagate ? " から" : " だけ"));
  if (record.saved_inputs)
    out += en ? " (with saved inputs)" : "（保存済みの入力で）";
  if (!ran.empty()) out += ": " + Names(ran, en);
  if (!failed.empty())
    out += (en ? " / failed: " : " / 失敗: ") + Names(failed, en);
  if (!record.never.empty())
    out += (en ? " / not run: " : " / 未実行: ") + Names(record.never, en);
  return out;
}
auto ChangeText(const Tsugumi::RPC::VersionChangesT& record, bool en)
    -> std::string {
  std::vector<std::string> entries;
  for (const auto& id : record.added)
    entries.push_back(std::string(en ? "Added " : "追加: ") + id);
  for (const auto& id : record.removed)
    entries.push_back(std::string(en ? "Removed " : "削除: ") + id);
  for (const auto& change : record.changed) {
    std::string entry = std::string(en ? "Changed " : "変更: ") +
                        (change->card.empty() ? "Workflow" : change->card) +
                        " (";
    for (const auto& kind : change->kinds) {
      std::string label = kind;
      if (!en) {
        if (kind == "definition")
          label = "定義";
        else if (kind == "parameters")
          label = "パラメータ";
        else if (kind == "source")
          label = "ソース";
        else if (kind == "wiring")
          label = "接続";
      }
      entry += (entry.back() == '(' ? "" : ", ") + label;
    }
    entries.push_back(entry + ")");
  }
  if (record.workflow)
    entries.push_back(en ? "Changed Workflow settings"
                         : "Workflow の設定を変更");
  std::string out;
  for (size_t i = 0; i < std::min(entries.size(), size_t{6}); ++i)
    out += (i ? "; " : "") + entries[i];
  if (entries.size() > 6)
    out += en ? "; and " + std::to_string(entries.size() - 6) + " more"
              : " ほか " + std::to_string(entries.size() - 6) + " 件";
  if (out.empty())
    out = en ? "No Card definition, source, parameter or wiring changes"
             : "Card の定義・ソース・パラメータ・接続の変更なし";
  return out;
}
auto PresentGraph(Tsugumi::RPC::VersionGraphT& graph, bool en) -> void {
  for (auto& commit : graph.commits) {
    if (commit->facts)
      commit->message = commit->summary = CommitText(*commit->facts, en);
    for (auto& run : commit->runs)
      run->summary = run->facts ? RunText(*run->facts, en) : "";
  }
}

}  // namespace tsugumi::host
