// node web/tools/module-perf.mjs <out> [baseline-ref]
// Builds a benchmark using either this checkout or a prior display/viewer implementation.
import * as esbuild from 'esbuild'
import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const root = path.dirname(web), out = process.argv[2], baseline = process.argv[3]
if (!out) throw Error('Pass an output directory and optional baseline git ref')
fs.mkdirSync(path.join(out, 'modules'), { recursive: true })
const read = file => baseline ? execFileSync('git', ['show', `${baseline}:${file}`], { cwd: root, encoding: 'utf8' }) : fs.readFileSync(path.join(root, file), 'utf8')
await esbuild.build({ entryPoints: [path.join(web, 'tools/module-perf.tsx')], outdir: out, bundle: true, format: 'esm', jsx: 'automatic', loader: { '.md': 'text' },
  plugins: baseline ? [{ name: 'baseline', setup(build) { build.onLoad({ filter: /\/ui\/display\.tsx$/ }, () => ({ contents: read('web/src/ui/display.tsx').replace('<iframe ref=', '<iframe sandbox="allow-scripts" ref='), loader: 'tsx', resolveDir: path.join(web, 'src/ui') })) } }] : [],
})
fs.writeFileSync(path.join(out, 'index.html'), '<!doctype html><meta charset="utf-8"><title>Module performance</title><link rel="stylesheet" href="module-perf.css"><div id="root"></div><script type="module" src="module-perf.js"></script>')
fs.writeFileSync(path.join(out, 'modules/viewer.html'), read('ui/modules/viewer.html').replace('<script src="viewer.js"></script>', '<script>' + read('ui/modules/viewer.js') + '</script>'))
for (const [name, from] of Object.entries({ 'plotly.min.js': 'plotly.js-dist-min/plotly.min.js', 'vega.min.js': 'vega/build/vega.min.js', 'vega-lite.min.js': 'vega-lite/build/vega-lite.min.js', 'vega-embed.min.js': 'vega-embed/build/vega-embed.min.js' })) fs.copyFileSync(path.join(web, 'node_modules', from), path.join(out, 'modules', name))
console.log(out)
