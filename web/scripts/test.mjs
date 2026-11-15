// Run behavioral regression tests without a browser.
import * as esbuild from 'esbuild'
import { execFileSync } from 'node:child_process'
import { preparePlayer } from '../../docs/site/player/build.mjs'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { pathToFileURL, fileURLToPath } from 'node:url'

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-behavior-'))
const out = path.join(dir, 'behavior.mjs')
try {
  await import('../tests/startup.mjs')
  if (!process.env.TSUGUMI_SITE_PLAYER_DIR) {
    const player = path.join(dir, 'player')
    execFileSync(process.execPath, [path.join(web, '../docs/site/player/build.mjs'), '--out-dir', player], { stdio: 'inherit' })
    process.env.TSUGUMI_SITE_PLAYER_DIR = player
  }
  preparePlayer()
  await esbuild.build({
    entryPoints: [path.join(web, 'tests', 'behavior.ts')], outfile: out,
    bundle: true, platform: 'node', format: 'esm', target: 'node22', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  process.argv = [process.argv[0], out, path.resolve(web, '..'), ...process.argv.slice(2)]
  await import(pathToFileURL(out).href)
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
