// Build a manual history-UI fixture without starting a daemon or changing a project.
import * as esbuild from 'esbuild'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const out = process.argv[2]
if (!out) throw Error('Usage: node web/scripts/environment-browser.mjs <output-directory>')
fs.mkdirSync(out, { recursive: true })
await esbuild.build({ entryPoints: [path.join(web, 'tests/environmentBrowser.tsx')], outdir: out,
  bundle: true, format: 'esm', loader: { '.md': 'text' }, jsx: 'automatic' })
fs.writeFileSync(path.join(out, 'index.html'), '<!doctype html><html lang="ja"><meta charset="utf-8"><title>Environment history QA</title><link rel="stylesheet" href="environmentBrowser.css"><div id="root"></div><script type="module" src="environmentBrowser.js"></script></html>')
console.log(`Serve ${out} on localhost, then open index.html.`)
