// Picks a Card's icon from a grid (search by words; no symbol names).
import { useRef, useState } from 'react'
import { Button, Popover, TextField } from './controls'
import { Icon } from './icons'
import { L } from '../i18n'

/** Icons a Card can use, with the words they are found by. Symbol names are
 *  internal and never shown or typed. */
export const CARD_SYMBOLS: { symbol: string; label: string }[] = ([
  ['chevron.left.forwardslash.chevron.right', L('コード 処理')], ['function', L('関数 計算')], ['sum', L('合計 集計')],
  ['plus.forwardslash.minus', L('四則演算 計算')], ['divide', L('割り算 半分')], ['multiply', L('掛け算 倍')],
  ['x.squareroot', L('平方根 数学')], ['percent', L('割合 パーセント')], ['number', L('数値 番号')],
  ['textformat', L('文字列 テキスト')], ['text.alignleft', L('テキスト 文章')], ['doc.text', L('文書 ファイル')],
  ['doc', L('ファイル')], ['folder', L('フォルダ ディレクトリ')], ['tray.and.arrow.down', L('受け取り 入力')],
  ['tray.and.arrow.up', L('送り出し 出力')], ['square.and.arrow.down', L('読み込み 取り込み')],
  ['square.and.arrow.up', L('書き出し 保存')], ['arrow.triangle.branch', L('分岐 分配')],
  ['arrow.triangle.merge', L('合流 まとめる')], ['arrow.triangle.swap', L('入れ替え 切り替え')],
  ['arrow.clockwise', L('繰り返し 再実行')], ['repeat', L('ループ 繰り返し')], ['shuffle', L('シャッフル 乱数')],
  ['line.3.horizontal.decrease', L('フィルタ 絞り込み')], ['arrow.up.arrow.down', L('並べ替え ソート')],
  ['magnifyingglass', L('検索 探す')], ['chart.xyaxis.line', L('折れ線 グラフ')], ['chart.bar', L('棒グラフ 集計')],
  ['chart.pie', L('円グラフ 割合')], ['tablecells', L('表 テーブル')], ['photo', L('画像 写真')], ['camera', L('カメラ 撮影')],
  ['cube', L('3D 立体')], ['map', L('地図')], ['location', L('位置 場所')], ['globe', L('ネットワーク 世界')],
  ['network', L('接続 通信')], ['server.rack', L('サーバ')], ['cylinder', L('データベース DB')],
  ['externaldrive', L('保存 ディスク')], ['archivebox', L('アーカイブ 保管')], ['shippingbox', L('パッケージ 荷物')],
  ['envelope', L('メール 送信')], ['bell', L('通知 アラート')], ['clock', L('時刻 時間')], ['timer', L('タイマー 計測')],
  ['calendar', L('日付 カレンダー')], ['hammer', L('ビルド 作る')], ['wrench.and.screwdriver', L('ツール 修理')],
  ['gearshape', L('設定 歯車')], ['slider.horizontal.3', L('パラメータ 調整')], ['play.circle', L('開始 再生')],
  ['stop.circle', L('停止 終了')], ['flag.checkered', L('完了 ゴール')], ['checkmark.seal', L('検証 確認')],
  ['exclamationmark.triangle', L('警告 注意')], ['lock', L('鍵 保護')], ['key', L('キー 認証')], ['person', L('人 ユーザー')],
  ['cpu', L('計算機 CPU')], ['memorychip', L('メモリ')], ['bolt', L('高速 電気')], ['leaf', L('自然 葉')], ['flame', L('ホット 炎')],
  ['drop', L('水 液体')], ['atom', L('科学 原子')], ['waveform', L('信号 波形')], ['speaker.wave.2', L('音声 スピーカー')],
  ['music.note', L('音楽')], ['film', L('動画 映画')], ['paintbrush', L('描画 絵')], ['pencil', L('編集 書く')],
  ['scissors', L('切り取り 分割')], ['link', L('リンク 参照')], ['paperclip', L('添付')], ['tag', L('タグ ラベル')],
  ['bookmark', L('ブックマーク 印')], ['star', L('お気に入り 星')], ['heart', L('重要 ハート')], ['list.bullet', L('リスト 一覧')],
  ['list.number', L('番号付き 順番')], ['square.grid.2x2', L('グリッド 格子')], ['square.stack.3d.up', L('階層 重ねる')],
  ['trash', L('削除 ゴミ箱')], ['ant', L('デバッグ 虫')], ['ladybug', L('バグ 不具合')], ['brain', L('推論 AI')],
  ['sparkles', L('生成 きらきら')], ['wand.and.stars', L('自動 魔法')], ['text.bubble', L('会話 メッセージ')],
  ['questionmark.circle', L('問い合わせ 質問')], ['info.circle', L('情報')], ['eye', L('表示 見る')],
  ['binoculars', L('観察 監視')], ['ruler', L('計測 長さ')], ['scalemass', L('重さ 質量')], ['thermometer.medium', L('温度')],
  ['airplane', L('移動 飛行機')], ['car', L('車 移動')], ['house', L('家 ホーム')], ['building.2', L('建物 組織')],
  ['dollarsign.circle', L('お金 金額')], ['cart', L('購入 カート')], ['creditcard', L('支払い カード')],
  ['dice', L('サイコロ 確率')], ['puzzlepiece', L('部品 プラグイン')], ['cloud', L('クラウド 雲')], ['sun.max', L('晴れ 太陽')],
  ['moon', L('夜 月')], ['bird', L('鳥 つぐみ')],
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
        <Button onClick={() => setOpen(!open)} help={L('アイコンを選ぶ')}>
          <span className="row gap6 center"><Icon name={current} size={16} />アイコンを選ぶ…</span>
        </Button>
      </span>
      {symbol !== undefined && <Button kind="link" help={L('Card の種類のアイコンを使います')} onClick={() => onChange(undefined)}>{L('デフォルトに戻す')}</Button>}
      <Popover open={open} onClose={() => setOpen(false)} anchor={anchor}>
        <div className="symbol-picker">
          <TextField value={search} onChange={setSearch} placeholder={L('探す（例: 計算、表、画像）')} autoFocus />
          <div className="symbol-grid" role="listbox" aria-label={L('アイコン')}>
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
          {!shown.length && <div className="caption secondary">{L('一致するアイコンはありません')}</div>}
          <div className="row center gap6">
            <Button disabled={symbol === undefined} onClick={() => { onChange(undefined); setOpen(false) }}>{L('デフォルトに戻す')}</Button>
            <span className="spacer" />
            <Icon name={inherited} className="secondary" />
            <span className="caption secondary">{L('デフォルト')}</span>
          </div>
        </div>
      </Popover>
    </div>
  )
}
