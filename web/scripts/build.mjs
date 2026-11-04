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
const copy = () => {
  fs.copyFileSync(path.join(root, 'src', 'index.html'), path.join(out, 'public', 'index.html'))
  fs.copyFileSync(path.join(root, '..', 'docs', 'assets', 'tsugumi-icon.svg'), path.join(out, 'public', 'icon.svg'))
  fs.copyFileSync(gen, path.join(out, 'tsugumi.bfbs'))
}
copy()

const server = {
  // The host server, and the Claude Code plugin's MCP server (plugins/tsugumi).
  entryPoints: { server: path.join(root, 'server', 'main.ts'), mcp: path.join(root, 'server', 'mcp.ts') },
  outdir: out,
  bundle: true, platform: 'node', format: 'esm', target: 'node22', sourcemap: true, loader: { '.md': 'text' },
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
