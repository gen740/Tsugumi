// node web/tools/evidence.mjs <output-directory>
// python3 -m http.server 8783 --bind 127.0.0.1 --directory <output-directory>
// Open the bands and spectrum result links to inspect the saved table and image.
import * as esbuild from 'esbuild'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const out = process.argv[2]
if (!out) throw Error('Pass an output directory')
fs.mkdirSync(out, { recursive: true })
await esbuild.build({ entryPoints: [path.join(web, 'tools/evidence.tsx')], outdir: out, bundle: true, format: 'esm', jsx: 'automatic', loader: { '.md': 'text', '.woff2': 'file', '.woff': 'file', '.ttf': 'file' } })
fs.writeFileSync(path.join(out, 'index.html'), '<!doctype html><meta charset="utf-8"><title>Evidence links</title><link rel="stylesheet" href="evidence.css"><div id="root"></div><script type="module" src="evidence.js"></script>')

fs.copyFileSync(path.join(web, '../docs/site/player-data/assets/casa-spectrum.png'), path.join(out, 'casa-spectrum.png'))
