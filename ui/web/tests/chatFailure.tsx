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
const detail = tree.root.findByType('pre').children.join('')
// The arguments and the result are laid out to read, never dropped.
assert.ok(detail.includes('"diagnostic": "RAW_DIAGNOSTIC"'))
assert.ok(detail.includes('"path": "definitions/share/_outputs.fbs"'))
act(() => { tree.unmount() })

// An approval says what will happen; the request as sent stays in a closed fold.
const { PermissionRequests } = await import('../src/ui/chat')
const { operationSummary, parseOperationApproval } = await import('../src/model/permissions')
const sources = 'project_access operation_approval\nproject_edit\n' + JSON.stringify({
  project: 'p', arguments_digest: 'DIGEST', permissions: ['nix_source heasoft github:gen740/heasoft', 'nix_source nixpkgs github:NixOS/nixpkgs/abc123'] })
const approval = parseOperationApproval([sources])!
assert.equal(approval.name, 'project_edit')
const lines = operationSummary(approval)
assert.equal(lines.length, 1)
assert.ok(lines[0].includes('heasoft (github:gen740/heasoft), nixpkgs (github:NixOS/nixpkgs/abc123)'), lines[0])
assert.ok(!lines[0].includes('DIGEST'))
const lock = parseOperationApproval(['project_access operation_approval\nnix_path_lock_update\n{"project":"p","revision":3}\nProposed arguments: {"name":"heasoft"}'])!
assert.equal(lock.args.name, 'heasoft')
assert.ok(operationSummary(lock)[0].includes('heasoft'))
assert.ok(lock.detail.includes('Proposed arguments: {\n  "name": "heasoft"\n}'))
let sheet!: ReturnType<typeof create>
act(() => { sheet = create(<PermissionRequests asked={[sources]} callId="call-1" />) })
const fold = sheet.root.findByType('details')
assert.equal(fold.props.open, undefined)
assert.ok(fold.findByType('pre').children.join('').includes('"arguments_digest": "DIGEST"'))
assert.ok(fold.findByType('pre').children.join('').startsWith('call-1\n'))
assert.ok(sheet.root.findAll(n => n.type === 'div' && n.children.join('').includes('heasoft (github:gen740/heasoft)')).length > 0)
act(() => { sheet.unmount() })
console.log('chat failure presentation: passed')
