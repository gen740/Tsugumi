// Bundles a check of the server and runs it against the repository's
// fixtures: server/contract.ts by default, server/chatgptCheck.ts with
// `chatgpt` (Sign in with ChatGPT against tests/fixtures/model_fixture.py).
import * as esbuild from 'esbuild'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { pathToFileURL, fileURLToPath } from 'node:url'

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-contract-'))
const out = path.join(dir, 'check.mjs')
const chatgpt = process.argv[2] === 'chatgpt'
if (chatgpt) process.argv.splice(2, 1)
try {
  await esbuild.build({
    entryPoints: [path.join(web, 'server', chatgpt ? 'chatgptCheck.ts' : 'contract.ts')], outfile: out,
    bundle: true, platform: 'node', format: 'esm', target: 'node22', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  process.argv = [process.argv[0], out, path.resolve(web, '..'), ...process.argv.slice(2)]
  await import(pathToFileURL(out).href)
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
