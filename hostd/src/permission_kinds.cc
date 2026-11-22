#include "host/permission_kinds.hh"

namespace tsugumi::host {
auto PermissionKinds(const std::string& language)
    -> Tsugumi::Host::PermissionKindsT {
  Tsugumi::Host::PermissionKindsT result;
  const bool en = language == "en";
  auto add = [&](const char* id, const char* ja, const char* english,
                 const char* icon) {
    auto kind = std::make_unique<Tsugumi::Service::PermissionKindT>();
    kind->id = id;
    kind->label = en ? english : ja;
    kind->icon = icon;
    result.kinds.push_back(std::move(kind));
  };
  add("network", "ネットワークへのアクセス", "Access the network", "network");
  add("sandbox_off", "ファイルシステムの隔離を解除",
      "Disable filesystem isolation", "shield.slash");
  add("sandbox_options", "プロセスの隔離や上限を緩和",
      "Relax process isolation or resource limits", "shield.slash");
  add("gui", "GUI へのアクセス", "Access the GUI", "macwindow");
  add("host_folder_read", "このマシンのフォルダを読む",
      "Read folders on this machine", "folder");
  add("host_folder_write", "このマシンのフォルダに書く",
      "Write to folders on this machine", "folder.badge.plus");
  add("nix_source", "外部の Nix ソースを読む", "Read an external Nix source",
      "shippingbox");
  add("execution_service", "実行サービスに処理とデータを送る",
      "Send computation and data to an execution service", "network");
  return result;
}
}  // namespace tsugumi::host
