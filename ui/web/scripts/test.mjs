// Run behavioral regression tests without a browser.
import * as esbuild from 'esbuild'
import { execFileSync } from 'node:child_process'
import { preparePlayer } from '../../../docs/site/player/build.mjs'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { pathToFileURL, fileURLToPath } from 'node:url'

// The developer's own keys never reach a test host.
for (const key of ['ANTHROPIC_API_KEY', 'OPENAI_API_KEY']) delete process.env[key]

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
  if (!process.env.TSUGUMI_SITE_PLAYER_DIR) {
    const player = path.join(dir, 'player')
    execFileSync(process.execPath, [path.join(web, '../../docs/site/player/build.mjs'), '--out-dir', player], { stdio: 'inherit' })
    process.env.TSUGUMI_SITE_PLAYER_DIR = player
  }
  preparePlayer()
  await esbuild.build({
    entryPoints: [path.join(web, 'tests', 'behavior.ts')], outfile: out,
    bundle: true, platform: 'node', format: 'esm', target: 'node22', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  process.argv = [process.argv[0], out, path.resolve(web, '../..'), ...process.argv.slice(2)]
  await import(pathToFileURL(out).href)
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
