import { fileURLToPath } from 'node:url'
import { defineConfig } from 'astro/config'
import { staticAssets } from './src/lib/static-assets.mjs'

export default defineConfig({
  site: 'https://gen740.github.io/Tsugumi/',
  output: 'static',
  outDir: '../../build/docs',
  cacheDir: '../../build/docs-cache',
  build: { format: 'file' },
  vite: { define: { 'process.env.TSUGUMI_DOCS_ROOT': JSON.stringify(fileURLToPath(new URL('../../', import.meta.url))) } },
  markdown: { syntaxHighlight: false },
  integrations: [staticAssets()],
})
