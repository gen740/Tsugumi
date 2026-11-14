// The space-management preview names only older folders, and deletion sends an
// explicit selection rather than asking the core to choose what is old.
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import * as R from '../src/gen/tsugumi/rpc'
import { ProjectSession } from '../src/model/session'
import { ProjectSettingsContent } from '../src/ui/projectViews'
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
assert.ok(html.includes(L('表示した scratch を削除…')))
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
