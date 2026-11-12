import SwiftUI
import TsugumiLauncher

/// Cache trust is an application setting shared by the VM, not project content.
struct BinaryCacheSettings: View {
  @Environment(AppModel.self) private var model
  @State private var caches: [BinaryCache] = []
  @State private var message: String?
  @State private var failed = false

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 16) {
        Text("Attic・Cachix などのバイナリキャッシュから、署名を検証してビルド済みのパッケージを取得します。登録した鍵は、この Mac の全プロジェクトの実行環境で信頼されます。")
          .font(.callout).foregroundStyle(.secondary)
        Text("登録したキャッシュには、全プロジェクトのビルドで必要になったパッケージのハッシュが問い合わされます。認証情報は、同じホストから固定ハッシュで取得するビルドにも使われます。")
          .font(.callout).foregroundStyle(.secondary)
        Text("保存した接続は、次に実行環境を起動したときに反映されます。保存しても実行中の Workflow は停止しません。実行環境は Tsugumi を終了すると停止するので、反映するには、実行が終わってから Tsugumi を終了して開き直してください。")
          .font(.callout).foregroundStyle(.secondary)
        Text("既定の cache.nixos.org と tsugumi.cachix.org は引き続き使用します。キャッシュへのアップロードは行いません。")
          .font(.caption).foregroundStyle(.secondary)
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
                  Text("認証情報はこの Mac のアプリ用ファイル（0600）に保存し、VM の Nix daemon だけに渡します。プロジェクトや実行記録には含めません。")
                    .font(.caption).foregroundStyle(.secondary)
                }.padding(.top, 8)
              }
            }.padding(6)
          }
        }
        HStack {
          Button { caches.append(BinaryCache()); message = nil } label: { Label("キャッシュ接続を追加", systemImage: "plus") }
          Spacer()
          Button("保存") {
            do {
              try BinaryCaches.save(caches, support: model.environments.support)
              failed = false
              message = String(localized: "保存しました。Tsugumi を終了して開き直すと、次の実行環境の起動から使用します。")
            } catch { failed = true; message = error.localizedDescription }
          }.buttonStyle(.borderedProminent)
        }
        if let message { Text(message).foregroundStyle(failed ? .red : .secondary).textSelection(.enabled) }
      }.padding()
    }
    .task {
      do { caches = try BinaryCaches.load(support: model.environments.support) }
      catch { failed = true; message = error.localizedDescription }
    }
  }
}
