import assert from 'node:assert/strict'
import { act, create } from 'react-test-renderer'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import type { ChatRow } from '../shared/chatPresentation'
import type { ProjectSession } from '../src/model/session'
import { language } from '../src/i18n'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { StructuredRow } = await import('../src/ui/chat')

// The host supplies the wording; the clients choose a language and keep the
// original receipt behind the details control, including in a folded step.
const raw = JSON.stringify({ reply: 'Failure', value: { kind: 'not_found', args: [{ key: 'path', value: 'definitions/share/_outputs.fbs' }], diagnostic: 'RAW_DIAGNOSTIC' } })
const failure: ChatRow = {
  id: 'call', kind: 'tool', role: '', text: '', name: 'project_file_read',
  arguments: '{"path":"definitions/share/_outputs.fbs"}', result: raw, state: 'failed', archived: false,
  summaryJa: 'HOST_SUMMARY_JA', summaryEn: 'HOST_SUMMARY_EN', scopeJa: '', scopeEn: '',
  instruction: '', failed: 0, recovered: 0, links: [], children: [],
}
const step: ChatRow = { ...failure, id: 'step', kind: 'step', name: '', arguments: '', result: '', failed: 1, children: [failure] }
const session = { displayPath: (p: string[]) => p.join('/') } as ProjectSession
const summary = language() === 'en' ? failure.summaryEn : failure.summaryJa
const html = renderToStaticMarkup(<StructuredRow row={step} session={session} />)
assert.ok(html.includes(summary))
assert.ok(!html.includes('RAW_DIAGNOSTIC'))
assert.ok(!html.includes('&quot;reply&quot;'))

let tree!: ReturnType<typeof create>
act(() => { tree = create(<StructuredRow row={step} session={session} />) })
assert.equal(tree.root.findAllByType('pre').length, 0)
assert.equal(tree.root.findByType('details').props.open, undefined)
act(() => { tree.root.findByType('button').props.onClick({ stopPropagation() {} }) })
const detail = tree.root.findByType('pre')
assert.ok(detail.children.join('').includes(raw))
assert.ok(detail.children.join('').includes(failure.arguments))
act(() => { tree.unmount() })
console.log('chat failure presentation: passed')
