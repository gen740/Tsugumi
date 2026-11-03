// Bundles server/contract.ts and runs it against the repository's fixtures.
import * as esbuild from 'esbuild'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { pathToFileURL, fileURLToPath } from 'node:url'

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-contract-'))
const out = path.join(dir, 'contract.mjs')
try {
  await esbuild.build({
    entryPoints: [path.join(web, 'server', 'contract.ts')], outfile: out,
    bundle: true, platform: 'node', format: 'esm', target: 'node22', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  process.argv = [process.argv[0], out, path.resolve(web, '..'), ...process.argv.slice(2)]
  await import(pathToFileURL(out).href)
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
