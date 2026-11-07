// Bundles the host server, the plugin's MCP server and the browser UI into dist/ with esbuild.
import * as esbuild from 'esbuild'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const out = path.join(root, 'dist')
const watch = process.argv.includes('--watch')
const gen = path.join(root, 'src', 'gen', 'tsugumi.bfbs')
if (!fs.existsSync(gen)) {
  console.error('src/gen is missing: run `nix develop --command npm run gen` first')
  process.exit(1)
}
fs.mkdirSync(path.join(out, 'public'), { recursive: true })
// The module viewer (ui/modules) and the plot libraries the page sends it,
// served from /modules/. The viewer is sandboxed and loads nothing itself, so
// its script goes inline.
const viewer = path.join(root, '..', 'ui', 'modules')
const MODULE_FILES = {
  'plotly.min.js': path.join(root, 'node_modules', 'plotly.js-dist-min', 'plotly.min.js'),
  'vega.min.js': path.join(root, 'node_modules', 'vega', 'build', 'vega.min.js'),
  'vega-lite.min.js': path.join(root, 'node_modules', 'vega-lite', 'build', 'vega-lite.min.js'),
  'vega-embed.min.js': path.join(root, 'node_modules', 'vega-embed', 'build', 'vega-embed.min.js'),
  // Their licenses travel with them (THIRD_PARTY_NOTICES.md).
  'LICENSE.plotly.js.txt': path.join(root, 'node_modules', 'plotly.js-dist-min', 'LICENSE'),
  'LICENSE.vega.txt': path.join(root, 'node_modules', 'vega', 'LICENSE'),
  'LICENSE.vega-lite.txt': path.join(root, 'node_modules', 'vega-lite', 'LICENSE'),
  'LICENSE.vega-embed.txt': path.join(root, 'node_modules', 'vega-embed', 'LICENSE'),
}
// The demo player's data (ui/demoPlayer.tsx, Home's thumbnails): each demo and the
// images it names, without what only builds that data.
const playerData = path.join(root, '..', 'docs', 'site', 'player-data')
const copy = () => {
  fs.copyFileSync(path.join(root, 'src', 'index.html'), path.join(out, 'public', 'index.html'))
  fs.mkdirSync(path.join(out, 'public', 'modules'), { recursive: true })
  for (const [name, from] of Object.entries(MODULE_FILES)) fs.copyFileSync(from, path.join(out, 'public', 'modules', name))
  const script = fs.readFileSync(path.join(viewer, 'viewer.js'), 'utf8')
  if (script.includes('</script')) throw new Error('viewer.js cannot be inlined: it contains </script')
  const page = fs.readFileSync(path.join(viewer, 'viewer.html'), 'utf8').replace('<script src="viewer.js"></script>', () => `<script>\n${script}</script>`)
  if (!page.includes(script)) throw new Error('viewer.html has no <script src="viewer.js"></script> to inline')
  fs.writeFileSync(path.join(out, 'public', 'modules', 'viewer.html'), page)
  fs.copyFileSync(path.join(root, '..', 'docs', 'assets', 'tsugumi-icon.svg'), path.join(out, 'public', 'icon.svg'))
  fs.cpSync(playerData, path.join(out, 'public', 'player-data'), {
    recursive: true,
    filter: (from) => !['index.json', 'ports.json'].includes(path.basename(from)) && !from.endsWith('.run.json'),
  })
  fs.copyFileSync(gen, path.join(out, 'tsugumi.bfbs'))
}
copy()

const server = {
  // The host server, and the Claude Code plugin's MCP server (plugins/tsugumi).
  entryPoints: { server: path.join(root, 'server', 'main.ts'), mcp: path.join(root, 'server', 'mcp.ts') },
  outdir: out,
  bundle: true, platform: 'node', format: 'esm', target: 'node22', sourcemap: true, loader: { '.md': 'text' },
  // A native module: loaded from node_modules when a terminal opens.
  external: ['node-pty'],
  logLevel: 'info',
}
const client = {
  entryPoints: { app: path.join(root, 'src', 'main.tsx') },
  outdir: path.join(out, 'public'),
  bundle: true, platform: 'browser', format: 'esm', target: 'es2022', sourcemap: true, minify: !watch,
  loader: { '.css': 'css' }, jsx: 'automatic', logLevel: 'info',
  define: { 'process.env.NODE_ENV': watch ? '"development"' : '"production"' },
}
if (watch) {
  for (const o of [server, client]) await (await esbuild.context(o)).watch()
} else {
  await Promise.all([esbuild.build(server), esbuild.build(client)])
}
