// Bundles the browser UI into dist/public (hostd serves it) with esbuild.
import * as esbuild from 'esbuild'
import { preparePlayer } from '../../../docs/site/player/build.mjs'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const out = path.join(root, 'dist')
const watch = process.argv.includes('--watch')
const gen = path.join(root, 'shared', 'gen', 'tsugumi.bfbs')
if (!fs.existsSync(gen)) {
  console.error('shared/gen is missing: run `nix develop --command npm run gen` first')
  process.exit(1)
}
if (!watch) fs.rmSync(out, { recursive: true, force: true })
fs.mkdirSync(path.join(out, 'public'), { recursive: true })
fs.cpSync(path.join(root, 'third-party'), path.join(out, 'licenses'), { recursive: true })
for (const component of ['flatbuffers', 'emscripten']) {
  fs.cpSync(path.join(root, '../../third-party', component), path.join(out, 'licenses', component), { recursive: true })
}
// The module viewer (ui/shared/modules) and the plot libraries the page sends it,
// served from /modules/. The viewer is sandboxed and loads nothing itself, so
// its script goes inline.
const viewer = path.join(root, '..', 'shared', 'modules')
// The demo player's data (ui/demoPlayer.tsx, Home's thumbnails): each demo and the
// images it names, without what only builds that data.
const playerData = path.join(preparePlayer(), 'player-data')
const copy = () => {
  fs.copyFileSync(path.join(root, 'src', 'index.html'), path.join(out, 'public', 'index.html'))
  fs.mkdirSync(path.join(out, 'public', 'modules'), { recursive: true })
  const script = fs.readFileSync(path.join(viewer, 'viewer.js'), 'utf8')
  if (script.includes('</script')) throw new Error('viewer.js cannot be inlined: it contains </script')
  const page = fs.readFileSync(path.join(viewer, 'viewer.html'), 'utf8').replace('<script src="viewer.js"></script>', () => `<script>\n${script}</script>`)
  if (!page.includes(script)) throw new Error('viewer.html has no <script src="viewer.js"></script> to inline')
  fs.writeFileSync(path.join(out, 'public', 'modules', 'viewer.html'), page)
  fs.copyFileSync(path.join(root, '../..', 'docs', 'assets', 'tsugumi-icon.svg'), path.join(out, 'public', 'icon.svg'))
  fs.rmSync(path.join(out, 'public', 'player-data'), { recursive: true, force: true })
  fs.cpSync(playerData, path.join(out, 'public', 'player-data'), {
    recursive: true,
    filter: (from) => !['index.json', 'ports.json'].includes(path.basename(from)) && !from.endsWith('.run.json'),
  })
  const client = process.env.TSUGUMI_WASM_CLIENT_DIR ?? path.join(root, '../..', 'build-wasm', 'client')
  fs.mkdirSync(path.join(out, 'public', 'client'), { recursive: true })
  for (const file of ['tsugumi-client.js', 'tsugumi-client.wasm']) {
    fs.copyFileSync(path.join(client, file), path.join(out, 'public', 'client', file))
  }
}
copy()

const client = {
  entryPoints: { app: path.join(root, 'src', 'main.tsx') },
  outdir: path.join(out, 'public'),
  metafile: true, bundle: true, platform: 'browser', format: 'esm', target: 'es2022', sourcemap: watch, legalComments: 'none', minify: !watch,
  loader: { '.css': 'css' }, jsx: 'automatic', logLevel: 'info',
  define: { 'process.env.NODE_ENV': watch ? '"development"' : '"production"' },
}
if (watch) await (await esbuild.context(client)).watch()
else await esbuild.build(client)
