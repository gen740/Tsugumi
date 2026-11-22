import assert from 'node:assert/strict'
import * as esbuild from 'esbuild'
import fs from 'node:fs/promises'
import path from 'node:path'
import { runInChromium } from './chromium.mjs'

export async function checkLogInChromium(browser, web, directory) {
  const html = path.join(directory, 'log-browser.html')
  await esbuild.build({
    entryPoints: [path.join(web, 'tests/logBrowser.tsx')], outfile: path.join(directory, 'log-browser.js'),
    bundle: true, jsx: 'automatic', logLevel: 'warning',
  })
  await fs.writeFile(html, `<!doctype html><link rel="stylesheet" href="log-browser.css">
    <div id="root" style="display:flex; width:320px; height:190px"></div><script src="log-browser.js"></script>`)
  assert.equal(await runInChromium(browser, html, 'globalThis.logBrowserResult'), null)
  console.log('Chromium log view: wrapped rows, scrolling, retention and coalesced following passed')
}
