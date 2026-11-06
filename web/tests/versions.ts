// Hovering a version says what its runs did: newest first, from the note.
import assert from 'node:assert/strict'

const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { runsHelp } = await import('../src/ui/versions')

assert.equal(runsHelp([]), 'この版での実行はありません。')
assert.equal(
  runsHelp([
    { state: 'failed', seconds: 1.25, summary: 'a だけ / 失敗: a（worker_failed）' },
    { state: 'succeeded', seconds: 12.3, summary: '全体: a、b ×2' },
  ]),
  '成功 · 12.3 秒 · 全体: a、b ×2\n失敗 · 1.3 秒 · a だけ / 失敗: a（worker_failed）',
)
// A run a demo's history lists has no summary.
assert.equal(runsHelp([{ state: 'succeeded', seconds: 2, summary: null }]), '成功 · 2.0 秒')
// Only the latest few: the rest are counted.
assert.equal(
  runsHelp(Array.from({ length: 7 }, (_, i) => ({ state: 'succeeded', seconds: i, summary: null }))).split('\n').at(-1),
  'ほか 2 回',
)
