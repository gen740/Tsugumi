// Run behavioral regression tests and, with a configured Chromium, the log viewport test.
import * as esbuild from 'esbuild'
import childProcess, { execFileSync } from 'node:child_process'
import { syncBuiltinESMExports } from 'node:module'
import assert from 'node:assert/strict'
import { preparePlayer } from '../../../docs/site/player/build.mjs'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { pathToFileURL, fileURLToPath } from 'node:url'

// Set before importing or bundling any server adapter, even outside the wrapper.
process.env.TSUGUMI_TEST_NO_SECRET_STORE = '1'
for (const key of ['ANTHROPIC_API_KEY', 'OPENAI_API_KEY']) delete process.env[key]
// An independent tripwire covers the entire suite, including future adapters.
// Record only command/argv, never stdin (which can contain a fixture key).
const securityCommands = []
for (const method of ['execFile', 'execFileSync', 'spawn', 'spawnSync']) {
  const original = childProcess[method]
  childProcess[method] = function (command, ...args) {
    if (path.basename(String(command)) === 'security') {
      securityCommands.push({ command, args: args[0] })
      throw new Error('Tests must inject a fake Keychain command runner')
    }
    return original.call(this, command, ...args)
  }
}
syncBuiltinESMExports()

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-behavior-'))
if (process.env.TSUGUMI_TEST_RUN_ROOT === undefined) {
  process.env.TSUGUMI_TEST_RUN_ROOT = dir
  const home = path.join(dir, 'home')
  fs.mkdirSync(home, { mode: 0o700 })
  for (const key of ['HOME', 'CFFIXED_USER_HOME', 'USERPROFILE']) process.env[key] = home
  process.env.TSUGUMI_SUPPORT_DIR = path.join(dir, 'support')
  delete process.env.TSUGUMI_HOST_STATE
  for (const [key, folder] of [['XDG_CACHE_HOME', 'cache'], ['XDG_CONFIG_HOME', 'config'], ['XDG_DATA_HOME', 'data']]) {
    process.env[key] = path.join(dir, folder)
  }
}
const out = path.join(dir, 'behavior.mjs')
try {
  await import('../tests/secretStore.mjs')
  await import('../tests/startup.mjs')
  if (!process.env.TSUGUMI_SITE_PLAYER_DIR) {
    const player = path.join(dir, 'player')
    execFileSync(process.execPath, [path.join(web, '../../docs/site/player/build.mjs'), '--out-dir', player], { stdio: 'inherit' })
    process.env.TSUGUMI_SITE_PLAYER_DIR = player
  }
  preparePlayer()
  await import('../tests/demoRecording.mjs')
  await esbuild.build({
    entryPoints: [path.join(web, 'tests', 'behavior.ts')], outfile: out,
    bundle: true, platform: 'node', format: 'esm', target: 'node22', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  process.argv = [process.argv[0], out, path.resolve(web, '../..'), ...process.argv.slice(2)]
  await import(pathToFileURL(out).href)
  if (process.env.TSUGUMI_CHROMIUM) {
    const { checkLogInChromium } = await import('../tests/logBrowser.mjs')
    await checkLogInChromium(process.env.TSUGUMI_CHROMIUM, web, dir)
  }
} finally {
  console.log(`Web suite: recorded Keychain OS commands ${JSON.stringify(securityCommands)}`)
  fs.rmSync(dir, { recursive: true, force: true })
  assert.deepEqual(securityCommands, [], 'Web tests reached the OS Keychain runner')
}
