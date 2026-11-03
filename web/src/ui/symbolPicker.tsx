// Picks a Card's icon from a grid (search by words; no symbol names).
import { useRef, useState } from 'react'
import { Button, Popover, TextField } from './controls'
import { Icon } from './icons'

/** Icons a Card can use, with the words they are found by. Symbol names are
 *  internal and never shown or typed. */
export const CARD_SYMBOLS: { symbol: string; label: string }[] = ([
  ['chevron.left.forwardslash.chevron.right', 'コード 処理'], ['function', '関数 計算'], ['sum', '合計 集計'],
  ['plus.forwardslash.minus', '四則演算 計算'], ['divide', '割り算 半分'], ['multiply', '掛け算 倍'],
  ['x.squareroot', '平方根 数学'], ['percent', '割合 パーセント'], ['number', '数値 番号'],
  ['textformat', '文字列 テキスト'], ['text.alignleft', 'テキスト 文章'], ['doc.text', '文書 ファイル'],
  ['doc', 'ファイル'], ['folder', 'フォルダ ディレクトリ'], ['tray.and.arrow.down', '受け取り 入力'],
  ['tray.and.arrow.up', '送り出し 出力'], ['square.and.arrow.down', '読み込み 取り込み'],
  ['square.and.arrow.up', '書き出し 保存'], ['arrow.triangle.branch', '分岐 分配'],
  ['arrow.triangle.merge', '合流 まとめる'], ['arrow.triangle.swap', '入れ替え 切り替え'],
  ['arrow.clockwise', '繰り返し 再実行'], ['repeat', 'ループ 繰り返し'], ['shuffle', 'シャッフル 乱数'],
  ['line.3.horizontal.decrease', 'フィルタ 絞り込み'], ['arrow.up.arrow.down', '並べ替え ソート'],
  ['magnifyingglass', '検索 探す'], ['chart.xyaxis.line', '折れ線 グラフ'], ['chart.bar', '棒グラフ 集計'],
  ['chart.pie', '円グラフ 割合'], ['tablecells', '表 テーブル'], ['photo', '画像 写真'], ['camera', 'カメラ 撮影'],
  ['cube', '3D 立体'], ['map', '地図'], ['location', '位置 場所'], ['globe', 'ネットワーク 世界'],
  ['network', '接続 通信'], ['server.rack', 'サーバ'], ['cylinder', 'データベース DB'],
  ['externaldrive', '保存 ディスク'], ['archivebox', 'アーカイブ 保管'], ['shippingbox', 'パッケージ 荷物'],
  ['envelope', 'メール 送信'], ['bell', '通知 アラート'], ['clock', '時刻 時間'], ['timer', 'タイマー 計測'],
  ['calendar', '日付 カレンダー'], ['hammer', 'ビルド 作る'], ['wrench.and.screwdriver', 'ツール 修理'],
  ['gearshape', '設定 歯車'], ['slider.horizontal.3', 'パラメータ 調整'], ['play.circle', '開始 再生'],
  ['stop.circle', '停止 終了'], ['flag.checkered', '完了 ゴール'], ['checkmark.seal', '検証 確認'],
  ['exclamationmark.triangle', '警告 注意'], ['lock', '鍵 保護'], ['key', 'キー 認証'], ['person', '人 ユーザー'],
  ['cpu', '計算機 CPU'], ['memorychip', 'メモリ'], ['bolt', '高速 電気'], ['leaf', '自然 葉'], ['flame', 'ホット 炎'],
  ['drop', '水 液体'], ['atom', '科学 原子'], ['waveform', '信号 波形'], ['speaker.wave.2', '音声 スピーカー'],
  ['music.note', '音楽'], ['film', '動画 映画'], ['paintbrush', '描画 絵'], ['pencil', '編集 書く'],
  ['scissors', '切り取り 分割'], ['link', 'リンク 参照'], ['paperclip', '添付'], ['tag', 'タグ ラベル'],
  ['bookmark', 'ブックマーク 印'], ['star', 'お気に入り 星'], ['heart', '重要 ハート'], ['list.bullet', 'リスト 一覧'],
  ['list.number', '番号付き 順番'], ['square.grid.2x2', 'グリッド 格子'], ['square.stack.3d.up', '階層 重ねる'],
  ['trash', '削除 ゴミ箱'], ['ant', 'デバッグ 虫'], ['ladybug', 'バグ 不具合'], ['brain', '推論 AI'],
  ['sparkles', '生成 きらきら'], ['wand.and.stars', '自動 魔法'], ['text.bubble', '会話 メッセージ'],
  ['questionmark.circle', '問い合わせ 質問'], ['info.circle', '情報'], ['eye', '表示 見る'],
  ['binoculars', '観察 監視'], ['ruler', '計測 長さ'], ['scalemass', '重さ 質量'], ['thermometer.medium', '温度'],
  ['airplane', '移動 飛行機'], ['car', '車 移動'], ['house', '家 ホーム'], ['building.2', '建物 組織'],
  ['dollarsign.circle', 'お金 金額'], ['cart', '購入 カート'], ['creditcard', '支払い カード'],
  ['dice', 'サイコロ 確率'], ['puzzlepiece', '部品 プラグイン'], ['cloud', 'クラウド 雲'], ['sun.max', '晴れ 太陽'],
  ['moon', '夜 月'], ['bird', '鳥 つぐみ'],
] as [string, string][]).map(([symbol, label]) => ({ symbol, label }))

/** `symbol`: the Card's own choice (undefined: the icon of its kind). */
export function SymbolPicker({ symbol, onChange, inherited }: { symbol?: string; onChange: (s: string | undefined) => void; inherited: string }) {
  const [open, setOpen] = useState(false)
  const [search, setSearch] = useState('')
  const anchor = useRef<HTMLSpanElement>(null)
  const current = symbol ?? inherited
  const q = search.toLowerCase()
  const shown = CARD_SYMBOLS.filter((s) => !q || s.label.toLowerCase().includes(q))
  return (
    <div className="row gap8 center">
      <span ref={anchor}>
        <Button onClick={() => setOpen(!open)} help="アイコンを選ぶ">
          <span className="row gap6 center"><Icon name={current} size={16} />アイコンを選ぶ…</span>
        </Button>
      </span>
      {symbol !== undefined && <Button kind="link" help="Card の種類のアイコンを使います" onClick={() => onChange(undefined)}>デフォルトに戻す</Button>}
      <Popover open={open} onClose={() => setOpen(false)} anchor={anchor}>
        <div className="symbol-picker">
          <TextField value={search} onChange={setSearch} placeholder="探す（例: 計算、表、画像）" autoFocus />
          <div className="symbol-grid" role="listbox" aria-label="アイコン">
            {shown.map((s) => (
              <button key={s.symbol} type="button" role="option" aria-selected={s.symbol === current} aria-label={s.label}
                title={s.label.split(' ')[0] ?? s.label} className={`symbol-cell${s.symbol === current ? ' selected' : ''}`}
                onClick={() => {
                  onChange(s.symbol)
                  setOpen(false)
                }}>
                <Icon name={s.symbol} size={16} />
              </button>
            ))}
          </div>
          {!shown.length && <div className="caption secondary">一致するアイコンはありません</div>}
          <div className="row center gap6">
            <Button disabled={symbol === undefined} onClick={() => { onChange(undefined); setOpen(false) }}>デフォルトに戻す</Button>
            <span className="spacer" />
            <Icon name={inherited} className="secondary" />
            <span className="caption secondary">デフォルト</span>
          </div>
        </div>
      </Popover>
    </div>
  )
}
