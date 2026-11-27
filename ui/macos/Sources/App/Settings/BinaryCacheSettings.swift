import SwiftUI
import TsugumiLauncher

/// Cache trust is an application setting shared by the VM, not project content.
struct BinaryCacheSettings: View {
  @Environment(AppModel.self) private var model
  @State private var caches: [BinaryCache] = []
  @State private var message: String?
  @State private var failed = false
  /// The list as hostd last stored it; edits that match it are not saved again.
  @State private var stored: [BinaryCache]?
  /// Counts edits, so only the last of quick successive edits is saved.
  @State private var edits = 0

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 16) {
        InfoHeader("キャッシュ接続", info: Text("Attic・Cachix などのバイナリキャッシュから、署名を検証してビルド済みのパッケージを取得します。登録した鍵は、この Mac の全プロジェクトの実行環境で信頼されます。"), Text("登録したキャッシュには、全プロジェクトのビルドで必要になったパッケージのハッシュが問い合わされます。認証情報は、同じホストから固定ハッシュで取得するビルドにも使われます。"), Text("保存した接続は、次に実行環境を起動したときに反映されます。保存しても実行中の Workflow は停止しません。実行環境は Tsugumi を終了すると停止するので、反映するには、実行が終わってから Tsugumi を終了して開き直してください。"), Text("既定の cache.nixos.org と tsugumi.cachix.org は引き続き使用します。キャッシュへのアップロードは行いません。"))
          .font(.subheadline)
        if caches.isEmpty, stored != nil {
          Label {
            Text("バイナリキャッシュ接続を登録すると、Attic・Cachix などのキャッシュからビルド済みのパッケージを取得し、同じものをこの Mac でビルドせずに済みます。取得したパッケージは、登録した公開鍵で署名を検証します。登録しなくても、既定の cache.nixos.org と tsugumi.cachix.org は使用します。")
              .fixedSize(horizontal: false, vertical: true)
          } icon: { Image(systemName: "shippingbox") }
            .font(.callout).foregroundStyle(.secondary)
        }
        ForEach($caches) { $cache in
          GroupBox {
            VStack(alignment: .leading, spacing: 8) {
              HStack {
                TextField("名前", text: $cache.name)
                Button(role: .destructive) { let id = cache.id; caches.removeAll { $0.id == id } } label: { Image(systemName: "trash") }
                  .help("キャッシュ接続を削除")
              }
              LabeledContent("キャッシュ URL") { TextField("https://cache.example.org/team", text: $cache.url).textContentType(.URL) }
              LabeledContent("署名の公開鍵") { TextField("cache-name:base64", text: $cache.publicKey).font(.system(.body, design: .monospaced)) }
              DisclosureGroup("認証（非公開キャッシュ）") {
                VStack(spacing: 8) {
                  LabeledContent("ユーザー名") { TextField("", text: $cache.username) }
                  LabeledContent("パスワード・token") { SecureField("", text: $cache.password) }
                  HStack { Spacer(); InfoButton("認証情報はこの Mac のアプリ用ファイル（0600）に保存し、VM の Nix daemon だけに渡します。プロジェクトや実行記録には含めません。") }
                }.padding(.top, 8)
              }
            }.padding(6)
          }
        }
        Button { caches.append(BinaryCache()); message = nil } label: { Label("キャッシュ接続を追加", systemImage: "plus") }
        if let message { Text(message).foregroundStyle(failed ? .red : .secondary).textSelection(.enabled) }
      }.padding()
    }
    .task {
      do {
        let loaded = try await BinaryCaches.load(support: model.environments.support)
        stored = loaded
        caches = loaded
      } catch { failed = true; message = error.localizedDescription }
    }
    .onChange(of: caches) { _, now in
      guard let stored, now != stored else { return }
      edits += 1
      let edit = edits
      Task {
        try? await Task.sleep(for: .milliseconds(600))
        guard edit == edits else { return }
        await save(now)
      }
    }
  }

  /// Each change is stored as it is made, like the other settings. A
  /// connection just added and still empty is not stored. hostd checks the
  /// whole list, so while a connection is being filled in nothing is stored
  /// and the message says why.
  @MainActor private func save(_ list: [BinaryCache]) async {
    let complete = list.filter { !$0.blank }
    if complete == (stored ?? []).filter({ !$0.blank }) { stored = list; return }
    do {
      try await BinaryCaches.save(complete, support: model.environments.support)
      stored = list
      failed = false
      message = String(localized: "保存しました。Tsugumi を終了して開き直すと、次の実行環境の起動から使用します。")
    } catch {
      failed = true
      message = String(localized: "この変更はまだ保存していません: ") + error.localizedDescription
    }
  }
}

private extension BinaryCache {
  var blank: Bool { name.isEmpty && url.isEmpty && publicKey.isEmpty && username.isEmpty && password.isEmpty }
}
