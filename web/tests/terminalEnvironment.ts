import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { terminalEnvironment } from '../server/terminalEnvironment'
const poisoned = ['OPENAI_API_KEY', 'ANTHROPIC_API_KEY', 'FUTURE_SECRET', 'BASH_ENV', 'ENV', 'NODE_OPTIONS', 'TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE']
const previous = poisoned.map((k) => process.env[k])
try {
  for (const k of poisoned) process.env[k] = 'host-secret'
  const env = terminalEnvironment('/project')
  assert.deepEqual(Object.keys(env).sort(), ['PATH', 'HOME', 'USER', 'LOGNAME', 'SHELL', 'TMPDIR', 'LANG', 'TERM', 'COLORTERM', 'TSUGUMI_PROJECT'].sort())
  // The child prints what it was given; no fixed path: a build sandbox has no /usr/bin/env.
  const child = spawnSync(process.execPath, ['-e', 'for (const [k, v] of Object.entries(process.env)) console.log(k + "=" + v)'], { env, encoding: 'utf8' })
  assert.equal(child.status, 0)
  assert.ok(!child.stdout.includes('host-secret'))
  assert.ok(child.stdout.includes('TSUGUMI_PROJECT=/project'))
} finally {
  poisoned.forEach((k, i) => { if (previous[i] === undefined) delete process.env[k]; else process.env[k] = previous[i] })
}
console.log('native terminal environment: passed')
