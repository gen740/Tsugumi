#include "commands.hh"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>

#include "agent_tools.hh"
#include "rpc.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::cli {
namespace f = std::filesystem;
namespace {

auto Words(const std::string& name) -> std::vector<std::string> {
  if (name == "nix_path_lock_update") return {"lock", "update"};
  std::vector<std::string> out;
  std::stringstream s(name);
  for (std::string word; std::getline(s, word, '_');) out.push_back(word);
  return out;
}

auto Spoken(const std::string& name) -> std::string {
  auto words = Words(name);
  std::string out;
  for (const auto& w : words) out += (out.empty() ? "" : " ") + w;
  return out;
}

auto Option(std::string field) -> std::string {
  std::replace(field.begin(), field.end(), '_', '-');
  return "--" + field;
}

/** The longest tool whose words begin the arguments. */
auto Find(const std::vector<ToolInfo>& tools, const std::vector<std::string>& args) -> std::pair<const ToolInfo*, size_t> {
  const ToolInfo* best = nullptr;
  size_t used = 0;
  for (const auto& tool : tools) {
    const auto words = Words(tool.name);
    if (words.size() <= used || words.size() > args.size()) continue;
    if (std::equal(words.begin(), words.end(), args.begin())) {
      best = &tool;
      used = words.size();
    }
  }
  return {best, used};
}

auto Type(const std::string& schema) -> std::string {
  try {
    return json::Text(json::Object(schema), "type");
  } catch (...) {
    return {};
  }
}

auto Help(const ToolInfo& tool) -> void {
  std::cout << "tsugumi " << Spoken(tool.name) << " [options]\n\n" << tool.description << "\n\n";
  const auto schema = json::Object(tool.input_schema);
  std::vector<std::string> required;
  try {
    for (const auto& r : json::Array(json::Get(schema, "required", "[]"))) required.push_back(json::String(r));
  } catch (...) {
  }
  const auto properties = json::Object(json::Get(schema, "properties", "{}"));
  if (!properties.empty()) std::cout << "Options:\n";
  for (const auto& [name, raw] : properties) {
    const auto property = json::Object(raw);
    auto type = json::Text(property, "type");
    if (type.empty()) type = "json";
    const bool needed = std::find(required.begin(), required.end(), name) != required.end();
    std::cout << "  " << Option(name) << " <" << type << ">" << (needed ? " (required)" : "") << '\n';
    if (const auto text = json::Text(property, "description"); !text.empty()) std::cout << "      " << text << '\n';
  }
  std::cout << "  --json <object>   all arguments as one JSON object\n";
  if (!tool.name.starts_with("agent_"))
    std::cout << "  --project <folder> the project (default: the folder with tsugumi.json at or above this one)\n";
  if (tool.name == "evaluate_nix")
    std::cout << R"(
Example:
  tsugumi evaluate nix --card '{"segments":["plot"]}' --expression '(import ./. { inherit params inputs; }).api' --build
)";
}

/** The tool's JSON arguments from --field value pairs, typed by its schema. */
auto Arguments(const ToolInfo& tool, const std::vector<std::string>& args, std::string& project, bool& help)
    -> std::string {
  const auto properties = json::Object(json::Get(json::Object(tool.input_schema), "properties", "{}"));
  json::Members out;
  std::map<std::string, std::vector<std::string>> lists;
  for (size_t i = 0; i < args.size(); ++i) {
    auto option = args[i];
    if (option == "--help" || option == "-h") {
      help = true;
      return "{}";
    }
    if (!option.starts_with("--")) throw std::runtime_error("unexpected argument " + option);
    std::optional<std::string> value;
    if (const auto eq = option.find('='); eq != std::string::npos) {
      value = option.substr(eq + 1);
      option = option.substr(0, eq);
    }
    auto name = option.substr(2);
    std::replace(name.begin(), name.end(), '-', '_');
    const auto take = [&]() -> std::string {
      if (value) return *value;
      if (++i == args.size()) throw std::runtime_error(option + " needs a value");
      return args[i];
    };
    if (name == "json") {
      for (const auto& [k, v] : json::Object(take())) out[k] = v;
      continue;
    }
    if (name == "project" && !properties.contains("project")) {
      project = take();
      continue;
    }
    const auto found = properties.find(name);
    if (found == properties.end()) throw std::runtime_error("unknown option " + option + " (see --help)");
    const auto property = json::Object(found->second);
    const auto type = json::Text(property, "type");
    if (type == "boolean") {
      if (!value && i + 1 < args.size() && (args[i + 1] == "true" || args[i + 1] == "false")) value = args[++i];
      out[name] = value ? (*value == "false" ? "false" : "true") : "true";
    } else if (type == "string") {
      out[name] = json::Quote(take());
    } else if (type == "integer" || type == "number") {
      const auto text = take();
      json::Object("{\"n\":" + text + "}");  // a number, or this throws
      out[name] = text;
    } else if (type == "array" && Type(json::Get(property, "items", "{}")) == "string") {
      lists[name].push_back(json::Quote(take()));
    } else {
      const auto text = take();
      // Objects, unions and lists of them are given as JSON.
      out[name] = text;
    }
  }
  for (auto& [name, items] : lists) out[name] = json::Encode(items);
  return json::Encode(out);
}

auto ProjectFolder(std::string given) -> std::string {
  if (!given.empty()) return f::absolute(given).lexically_normal().string();
  for (auto dir = f::current_path(); !dir.empty(); dir = dir.parent_path()) {
    if (f::exists(dir / "tsugumi.json")) return dir.string();
    if (dir == dir.parent_path()) break;
  }
  throw std::runtime_error("not in a Tsugumi project: give --project <folder>");
}

/** The families the `tools` tool reaches (its schema's family enum). */
auto Families(const std::vector<ToolInfo>& catalog) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& tool : catalog) {
    if (tool.name != "tools") continue;
    try {
      const auto family = json::Object(json::Get(json::Object(json::Get(json::Object(tool.input_schema), "properties", "{}")), "family", "{}"));
      for (const auto& name : json::Array(json::Get(family, "enum", "[]"))) out.push_back(json::String(name));
    } catch (...) {
    }
  }
  return out;
}

/** A family's actions, from the `tools` tool on the current project. */
auto Actions(AgentTools& tools, const std::string& family) -> std::vector<ToolInfo> {
  const auto listed = tools.Call("tools", json::Encode(json::Members{{"family", json::Quote(family)}}));
  if (listed.error) throw std::runtime_error(listed.text);
  std::vector<ToolInfo> out;
  for (const auto& raw : json::Array(listed.text)) {
    const auto action = json::Object(raw);
    out.push_back({family + "_" + json::Text(action, "action"), json::Text(action, "description"),
                   json::Get(action, "input_schema", "{}")});
  }
  return out;
}

/** --project, wherever it is among the arguments. */
auto ProjectOption(const std::vector<std::string>& args) -> std::string {
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == "--project") return args[i + 1];
  for (const auto& a : args)
    if (a.starts_with("--project=")) return a.substr(10);
  return {};
}

}  // namespace

auto PrintTools(const f::path& executable) -> void {
  AgentTools tools(LaunchFromEnvironment(executable));
  std::vector<ToolInfo> catalog;
  try {
    catalog = tools.Catalog();
  } catch (const std::exception& error) {
    std::cout << "\n(The tools are listed by a running host: " << error.what() << ")\n";
    return;
  }
  std::cout << "\nTools (`tsugumi <tool> --help` shows its options):\n";
  for (const auto& family : Families(catalog))
    std::cout << "  " << family << " <action> - the " << family << " family (`tsugumi " << family
              << " --help` lists its actions)\n";
  for (const auto& tool : catalog) {
    if (tool.name == "tools") continue;
    auto summary = tool.description.substr(0, tool.description.find('\n'));
    if (summary.size() > 90) summary = summary.substr(0, 87) + "...";
    std::cout << "  " << Spoken(tool.name) << (summary.empty() ? "" : " - " + summary) << '\n';
  }
}

auto RunTool(const f::path& executable, const std::vector<std::string>& args) -> std::optional<int> {
  AgentTools tools(LaunchFromEnvironment(
      executable, args.empty() ? "tsugumi" : "tsugumi " + args.front()));
  const auto catalog = tools.Catalog();
  const auto families = Families(catalog);
  if (!args.empty() && std::find(families.begin(), families.end(), args[0]) != families.end()) {
    // A family action (`tsugumi runs status ...`): its schema comes from the
    // project's `tools` listing, and the call goes through `tools`.
    const auto& family = args[0];
    tools.Show(ProjectFolder(ProjectOption(args)), false, false);
    const auto actions = Actions(tools, family);
    const auto [action, used] = Find(actions, args);
    if (!action) {
      std::cout << "tsugumi " << family << " <action> [options]\n\nActions:\n";
      for (const auto& a : actions)
        std::cout << "  " << Spoken(a.name) << " - " << a.description.substr(0, a.description.find('\n')) << '\n';
      return args.size() == 1 || args[1] == "--help" || args[1] == "-h" ? 0 : 64;
    }
    std::string project;
    bool help = false;
    const auto arguments = Arguments(*action, {args.begin() + static_cast<long>(used), args.end()}, project, help);
    if (help) {
      Help(*action);
      return 0;
    }
    const auto name = action->name.substr(family.size() + 1);
    const auto result = tools.Call("tools", json::Encode(json::Members{{"family", json::Quote(family)},
                                                                      {"action", json::Quote(name)},
                                                                      {"arguments", arguments}}));
    std::cout << result.text << '\n';
    if (!result.images.empty()) std::cerr << result.images.size() << " image(s) not shown\n";
    return result.error ? 1 : 0;
  }
  const auto [tool, used] = Find(catalog, args);
  if (!tool) return std::nullopt;
  std::string project;
  bool help = false;
  const auto arguments = Arguments(*tool, {args.begin() + static_cast<long>(used), args.end()}, project, help);
  if (help) {
    Help(*tool);
    return 0;
  }
  if (!tool->name.starts_with("agent_")) tools.Show(ProjectFolder(project), false, false);
  const auto result = tools.Call(tool->name, arguments);
  std::cout << result.text << '\n';
  if (!result.images.empty()) std::cerr << result.images.size() << " image(s) not shown\n";
  return result.error ? 1 : 0;
}

auto RunCall(const f::path& executable, const std::vector<std::string>& args) -> int {
  if (args.empty() || args.size() > 2) {
    std::cerr << "Usage: tsugumi call <RequestName> ['<json>']\n";
    return 64;
  }
  AgentTools tools(LaunchFromEnvironment(executable, "tsugumi call"));
  try {
    std::cout << tools.Request(args[0], args.size() == 2 ? args[1] : "{}") << '\n';
    return 0;
  } catch (const RpcFailure& e) {
    std::cout << json::Encode(json::Members{{"code", json::Quote(e.code)},
                                            {"message", json::Quote(e.what())},
                                            {"request_id", json::Quote(e.request_id)}})
              << '\n';
    return 1;
  }
}

}  // namespace tsugumi::cli
