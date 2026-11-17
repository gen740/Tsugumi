#include "agent/context.hh"

#include <algorithm>
#include <exception>
#include <map>

#include "agent/catalog.hh"
#include "util/json_object.hh"

namespace tsugumi::agent {
namespace {
// A `tools` call runs the family action it names, so its result has that
// tool's shape. Anything else, including a schema listing, keeps its own name.
auto ResultTool(const ContentBlock& call) -> std::string {
  if (call.name != "tools") return call.name;
  static const auto families = json::Object(json::Get(
      json::Object(json::Get(Catalog().shared(), "modelTools")), "families"));
  try {
    const auto args = json::Object(call.input);
    const auto family = families.find(json::Text(args, "family"));
    if (family == families.end()) return call.name;
    const auto name =
        json::Text(json::Object(family->second), json::Text(args, "action"));
    return name.empty() ? call.name : name;
  } catch (const std::exception&) {
    return call.name;
  }
}
}  // namespace

auto ClipMiddle(std::string_view text, size_t limit, std::string_view reference)
    -> std::string {
  if (text.size() <= limit) return std::string(text);
  const auto overhead = 128 + reference.size();
  const auto room = limit > overhead ? limit - overhead : 0;
  size_t head = room / 2;
  size_t tail = text.size() - (room - head);
  while (head && (static_cast<unsigned char>(text[head]) & 0xc0) == 0x80)
    --head;
  while (tail < text.size() &&
         (static_cast<unsigned char>(text[tail]) & 0xc0) == 0x80)
    ++tail;
  const auto exact = "\n[Omitted bytes [" + std::to_string(head) + "," +
                     std::to_string(tail) + ") (" +
                     std::to_string(tail - head) + " bytes); " +
                     std::string(reference) + "]\n";
  return std::string(text.substr(0, head)) + exact +
         std::string(text.substr(tail));
}

auto RequestMessages(const std::vector<Message>& messages,
                     size_t omitted_before, size_t result_limit)
    -> std::vector<Message> {
  auto view = messages;
  std::map<std::string, ContentBlock> calls;
  for (size_t i = 0; i < view.size(); ++i) {
    for (auto& block : view[i].content) {
      if (block.type == "tool_use") calls[block.id] = block;
      if (block.type != "tool_result") continue;
      const auto reference =
          "read the saved result with tool_result_read " +
          json::Encode(json::Members{
              {"call_id", json::Quote(block.id)},
              {"offset", json::Quote("0")},
              {"limit_bytes", std::to_string(kResultPageBytes)}}) +
          "; offset is a byte offset in the original result; advance by "
          "next_offset";
      if (i < omitted_before) {
        const auto& call = calls[block.id];
        block.text = "[Earlier result omitted: " + call.name + " " +
                     ClipMiddle(call.input, 512, "arguments abbreviated") +
                     "; " + reference + "]";
        block.images.clear();
      } else {
        const auto name = ResultTool(calls[block.id]);
        if (!block.is_error &&
            (name == "project_shell" || name == "project_file_read" ||
             name == "attached_folder_read"))
          block.text = FileResultForModel(block.text, result_limit, reference);
        else if (name != "input_types")
          block.text = ClipMiddle(block.text, result_limit, reference);
      }
    }
  }
  return view;
}

auto RemovedMessages(const std::vector<Message>& before,
                     const std::vector<Message>& after)
    -> std::vector<Message> {
  const auto retains = [](const Message& old, Message candidate) {
    if (candidate.content.size() < old.content.size()) return false;
    candidate.content.resize(old.content.size());
    return SerializeMessages({candidate}) == SerializeMessages({old});
  };
  // Ordinary append checkpoints (including additional parallel tool blocks)
  // retain their prefix. Match the remainder from the end: identical assistant
  // answers must correspond to the retained suffix, not an earlier occurrence.
  size_t prefix = 0;
  while (prefix < before.size() && prefix < after.size() &&
         retains(before[prefix], after[prefix]))
    ++prefix;
  std::vector<bool> kept(before.size(), false);
  std::fill(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(prefix),
            true);
  size_t end = after.size();
  for (size_t i = before.size(); i > prefix;) {
    --i;
    size_t candidate = end;
    while (candidate > prefix && !retains(before[i], after[candidate - 1]))
      --candidate;
    if (candidate > prefix) {
      kept[i] = true;
      end = candidate - 1;
    }
  }
  std::vector<Message> removed;
  for (size_t i = 0; i < before.size(); ++i)
    if (!kept[i]) removed.push_back(before[i]);
  return removed;
}
}  // namespace tsugumi::agent
