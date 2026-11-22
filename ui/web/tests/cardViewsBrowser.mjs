import assert from 'node:assert/strict'
import * as esbuild from 'esbuild'
import fs from 'node:fs/promises'
import path from 'node:path'
import { installViewer } from '../scripts/install-viewer.mjs'
import { runInChromium } from './chromium.mjs'

export async function checkCardViewsInChromium(browser, web, directory) {
  const modules = path.join(directory, 'modules')
  // Keep both fresh installs and upgrades free of bundled views.
  await fs.mkdir(modules)
  await fs.writeFile(path.join(modules, 'legacy.js'), '')
  installViewer(path.join(web, '../shared/modules'), modules)
  assert.deepEqual(await fs.readdir(modules), ['viewer.html'])
  if (process.env.TSUGUMI_WEB_PUBLIC_DIR) {
    const distribution = path.join(process.env.TSUGUMI_WEB_PUBLIC_DIR, 'modules')
    assert.deepEqual(await fs.readdir(distribution), ['viewer.html'])
    assert.deepEqual(await fs.readFile(path.join(distribution, 'viewer.html')), await fs.readFile(path.join(modules, 'viewer.html')))
  }

  const html = path.join(directory, 'card-views.html')
  await esbuild.build({
    entryPoints: [path.join(web, 'tests/cardViewsBrowser.tsx')], outfile: path.join(directory, 'card-views.js'),
    bundle: true, jsx: 'automatic', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  await fs.writeFile(html, '<!doctype html><div id="root"></div><script src="card-views.js"></script>')
  assert.equal(await runInChromium(browser, html, 'globalThis.cardViewsResult'), null)
  console.log('Chromium Card views: empty frames, exact bytes, controls, CSP, diagnostics and teardown passed')
}
