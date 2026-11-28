// Run scratch is erased when each invocation ends, so the cache pane reports
// and deletes build caches only.
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import * as R from '../shared/gen/tsugumi/rpc'
import { ProjectSession, formatBytes } from '../src/model/session'
import { ProjectSettingsContent } from '../src/ui/projectViews'
import { L } from '../src/i18n'

const usage = new R.ProjectCacheUsageResultT()
usage.currentProject = 'project'
const project = new R.ProjectCacheUsageT()
project.project = 'project'
project.name = 'Example'
project.buildBytes = 4096n
usage.projects = [project]
const fake = { cacheUsage: usage, key: 'project', connected: true, runActive: false } as unknown as ProjectSession
const html = renderToStaticMarkup(<ProjectSettingsContent pane="cache" session={fake} />)
assert.ok(html.includes('Example'))
assert.ok(html.includes(L('ビルドキャッシュ')))
assert.ok(html.includes(formatBytes(4096n)))
assert.ok(!html.includes('scratch'))
console.log('cache pane shows build caches without run scratch: passed')
