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
  assert.deepEqual(await fs.readdir(modules), ['viewer.html', 'worker-dashboard.html'])
  if (process.env.TSUGUMI_WEB_PUBLIC_DIR) {
    const distribution = path.join(process.env.TSUGUMI_WEB_PUBLIC_DIR, 'modules')
    assert.deepEqual(await fs.readdir(distribution), ['viewer.html', 'worker-dashboard.html'])
    for (const name of ['viewer.html', 'worker-dashboard.html']) {
      assert.deepEqual(await fs.readFile(path.join(distribution, name)), await fs.readFile(path.join(modules, name)))
    }
  }

  const html = path.join(directory, 'card-views.html')
  await esbuild.build({
    entryPoints: [path.join(web, 'tests/cardViewsBrowser.tsx')], outfile: path.join(directory, 'card-views.js'),
    bundle: true, jsx: 'automatic', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  await fs.writeFile(html, '<!doctype html><link rel="stylesheet" href="card-views.css"><div id="root"></div><script src="card-views.js"></script>')
  assert.equal(await runInChromium(browser, html, 'globalThis.cardViewsResult', ['--disable-gpu']), null)
  console.log('Chromium Card views: sessions, in-page windows, detach, minimize, exact bytes, full screen, sandbox and teardown passed')
}

export async function checkDawnWorkerInChromium(browser, web, directory) {
  const html = path.join(directory, 'dawn-worker.html')
  await esbuild.build({
    entryPoints: {
      'dawn-worker-test': path.join(web, 'tests/dawnWorkerBrowser.tsx'),
      'dawn-view-worker': path.join(web, 'src/dawn/viewWorker.ts'),
      'dawn-view-audio': path.join(web, 'src/dawn/audioWorklet.ts'),
    },
    outdir: directory, platform: 'browser', format: 'esm', bundle: true, jsx: 'automatic',
    loader: { '.md': 'text' }, logLevel: 'warning',
  })
  await fs.writeFile(html, '<!doctype html><script type="module" src="dawn-worker-test.js"></script>')
  const result = await runInChromium(browser, html, 'globalThis.dawnWorkerResult', [
    '--enable-unsafe-webgpu', '--allow-file-access-from-files',
    '--autoplay-policy=no-user-gesture-required',
  ])
  assert.equal(result, null)
  console.log('Chromium Dawn views: OffscreenCanvas, draw paused while minimized, IME, AudioWorklet, PNG snapshot, waiting overlay and Stop passed')
}
