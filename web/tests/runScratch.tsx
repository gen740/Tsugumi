import EN from '../src/i18n/en.json'
import { Children, isValidElement, ReactElement } from 'react'
import { Button, Toggle, Value } from '../src/ui/controls'
// The space-management preview names only older folders, and deletion sends an
// explicit selection rather than asking the core to choose what is old.
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import * as R from '../src/gen/tsugumi/rpc'
import { ProjectSession, formatBytes } from '../src/model/session'
import { ProjectSettingsContent, ScratchRows } from '../src/ui/projectViews'
import { L } from '../src/i18n'

const usage = new R.ProjectCacheUsageResultT()
usage.currentProject = 'project'
usage.protectedRun = 'latest-run'
const project = new R.ProjectCacheUsageT()
project.project = 'project'
project.name = 'Example'
project.scratchBytes = 1015n
project.runScratch = [new R.RunScratchUsageT('runs/old-run', 1000n), new R.RunScratchUsageT('runs/latest-run', 10n), new R.RunScratchUsageT('recovery', 5n)]
usage.projects = [project]
const fake = { cacheUsage: usage, key: 'project', connected: true, runActive: false } as unknown as ProjectSession
const html = renderToStaticMarkup(<ProjectSettingsContent pane="cache" session={fake} />)
assert.ok(html.includes('runs/old-run'))
assert.ok(html.includes('recovery'))
assert.ok(!html.includes('runs/latest-run'))
assert.ok(html.includes(L('選択した scratch を削除…')))
assert.ok(html.includes(L('削除対象の合計')))
let requested: string[] | undefined
let refreshed = false
const result = new R.RunScratchDeleteResultT(['runs/old-run'], [], 1000n)
const model = {
  client: { async deleteRunScratch(_opId: string, paths: string[]) { requested = paths; return result } },
  async refreshCacheUsage() { refreshed = true },
} as unknown as ProjectSession
assert.equal(await ProjectSession.prototype.deleteRunScratch.call(model, ['runs/old-run']), result)
assert.deepEqual(requested, ['runs/old-run'])
assert.ok(refreshed)
console.log('run scratch preview and explicit selection: passed')

// Exercise the actual row and deletion callbacks, rather than handing the
// model a subset constructed by the test.
const selected = new Set<string>()
const unknown = Object.assign(new R.RunScratchUsageT('unreadable', 99n), { sizeUnknown: true })
const candidates = [project.runScratch[0], project.runScratch[2], unknown]
let picked: string[] | undefined
const rows = () => ScratchRows({ candidates, selected, onSelect(path, on) { if (on) selected.add(path); else selected.delete(path) }, onDelete(paths) { picked = paths } })
const elements = (node: unknown): ReactElement<any>[] => Children.toArray(node as any).flatMap(node => {
  if (!isValidElement<any>(node)) return []
  return [node, ...elements(node.props.children)]
})
const toggle = (path: string, on: boolean) => elements(rows()).find(e => e.type === Toggle && e.props.label === path)!.props.onChange(on)
const remove = () => elements(rows()).find(e => e.type === Button)!
const total = () => elements(rows()).find(e => e.type === Value && e.props.label === L('削除対象の合計'))!.props.value
assert.equal(remove().props.disabled, true)
assert.equal(total(), formatBytes(0n))
toggle('recovery', true)
assert.equal(total(), formatBytes(5n))
toggle('unreadable', true)
assert.equal(total(), formatBytes(5n), 'unknown bytes are excluded from the total')
let shown = renderToStaticMarkup(rows())
assert.ok(shown.includes(L('不明')))
assert.ok(shown.includes(L('合計は容量不明のフォルダを除きます。')))
assert.ok(!shown.includes(formatBytes(99n)))
toggle('runs/old-run', true)
assert.equal(total(), formatBytes(1005n))
toggle('runs/old-run', false)
assert.equal(total(), formatBytes(5n))
assert.equal(remove().props.disabled, false)
remove().props.onClick()
assert.deepEqual(picked, ['recovery', 'unreadable'])
await ProjectSession.prototype.deleteRunScratch.call(model, picked!)
assert.deepEqual(requested, ['recovery', 'unreadable'])
console.log('scratch row controls, selected bytes, unknown sizes and delete selection: passed')

assert.equal(EN['不明'], 'Unknown')
assert.equal(EN['合計は容量不明のフォルダを除きます。'], 'The total excludes folders with unknown sizes.')
