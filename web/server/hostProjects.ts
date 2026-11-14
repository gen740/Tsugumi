import { L } from './i18n'

// Presentation only: the host core decides which failure applies.
export function hostProjectErrorText(code: string): string {
  switch (code) {
    case 'invalid_project_name': return L('フォルダ名に使えない文字が含まれているか、名前が空です。')
    case 'project_defaults_unavailable': return L('このビルドには既定の Nixpkgs が設定されていません。')
    case 'invalid_flow': return L('フローの方向が正しくありません。')
    case 'folder_not_empty': return L('作成先は空ではありません。空のフォルダか新しい名前を選んでください。')
    case 'project_too_large': return L('プロジェクトが大きすぎます（64 MB まで）。')
    case 'invalid_project': return L('有効なプロジェクトのフォルダを指定してください。複製先は元のフォルダの外にしてください。')
    case 'demo_not_found': return L('デモが見つかりません。')
    case 'invalid_language': return L('デモの言語が正しくありません。')
    case 'permission_denied': return L('この操作にはユーザーの権限が必要です。')
    default: return L('プロジェクトの操作に失敗しました。')
  }
}
