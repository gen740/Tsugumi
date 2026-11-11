// Build a manual fixture without starting a daemon or changing a project.
import * as esbuild from 'esbuild'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const out = process.argv[2]
if (!out) throw Error('Usage: node web/scripts/nix-reference-browser.mjs <output-directory>')
fs.mkdirSync(out, { recursive: true })
await esbuild.build({ entryPoints: [path.join(web, 'tests/nixReferenceBrowser.tsx')], outdir: out,
  bundle: true, format: 'esm', loader: { '.md': 'text' }, jsx: 'automatic' })
fs.writeFileSync(path.join(out, 'index.html'), '<!doctype html><html lang="ja"><meta charset="utf-8"><title>Nix references QA</title><link rel="stylesheet" href="nixReferenceBrowser.css"><div id="root"></div><script type="module" src="nixReferenceBrowser.js"></script></html>')
console.log(`Serve ${out} on localhost, then open index.html.`)
