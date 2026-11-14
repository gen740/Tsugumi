// Run behavioral regression tests without a browser.
import * as esbuild from 'esbuild'
import { preparePlayer } from '../../docs/site/player/build.mjs'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { pathToFileURL, fileURLToPath } from 'node:url'

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
await import('../tests/startup.mjs')
process.env.TSUGUMI_SITE_PLAYER_DIR = preparePlayer()
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-behavior-'))
const out = path.join(dir, 'behavior.mjs')
try {
  await esbuild.build({
    entryPoints: [path.join(web, 'tests', 'behavior.ts')], outfile: out,
    bundle: true, platform: 'node', format: 'esm', target: 'node22', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  process.argv = [process.argv[0], out, path.resolve(web, '..'), ...process.argv.slice(2)]
  await import(pathToFileURL(out).href)
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
