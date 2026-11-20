import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import * as esbuild from 'esbuild'
import fs from 'node:fs/promises'
import path from 'node:path'
import { pathToFileURL } from 'node:url'

export async function checkLogInChromium(browser, web, directory) {
  const html = path.join(directory, 'log-browser.html')
  await esbuild.build({
    entryPoints: [path.join(web, 'tests/logBrowser.tsx')], outfile: path.join(directory, 'log-browser.js'),
    bundle: true, jsx: 'automatic', logLevel: 'warning',
  })
  await fs.writeFile(html, `<!doctype html><link rel="stylesheet" href="log-browser.css">
    <div id="root" style="display:flex; width:320px; height:190px"></div><script src="log-browser.js"></script>`)
  const proc = spawn(browser, [
    '--headless', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking', '--use-mock-keychain',
    `--user-data-dir=${path.join(directory, 'browser-profile')}`, '--remote-debugging-pipe', 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe'] })
  const closed = new Promise((resolve) => proc.once('close', resolve))
  let stderr = ''
  proc.stderr.setEncoding('utf8').on('data', (data) => { stderr += data })
  const pending = new Map()
  const events = new Map()
  let sequence = 0
  let buffer = ''
  proc.stdio[4].setEncoding('utf8').on('data', (data) => {
    buffer += data
    let split
    while ((split = buffer.indexOf('\0')) >= 0) {
      const message = JSON.parse(buffer.slice(0, split))
      buffer = buffer.slice(split + 1)
      if (message.id) {
        const task = pending.get(message.id)
        pending.delete(message.id)
        if (message.error) task.reject(new Error(message.error.message))
        else task.resolve(message.result)
      } else {
        const event = `${message.sessionId}:${message.method}`
        events.get(event)?.()
        events.delete(event)
      }
    }
  })
  const send = (method, params, sessionId) => new Promise((resolve, reject) => {
    const id = ++sequence
    pending.set(id, { resolve, reject })
    proc.stdio[3].write(JSON.stringify({ id, method, params, sessionId }) + '\0')
  })
  // A missing browser or event fails the suite. No assertion uses this duration.
  let watchdog
  const stopped = new Promise((_, reject) => {
    watchdog = setTimeout(() => reject(new Error(`Chromium log test did not finish\n${stderr}`)), 45000)
    proc.once('error', reject)
    proc.once('exit', (code) => reject(new Error(`Chromium exited with ${code}\n${stderr}`)))
    proc.stdio[3].on('error', reject)
  })
  try {
    await Promise.race([stopped, (async () => {
      const { targetId } = await send('Target.createTarget', { url: 'about:blank' })
      const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true })
      await send('Page.enable', {}, sessionId)
      const loaded = new Promise((resolve) => events.set(`${sessionId}:Page.loadEventFired`, resolve))
      const navigation = await send('Page.navigate', { url: pathToFileURL(html).href }, sessionId)
      assert.equal(navigation.errorText, undefined)
      await loaded
      const reply = await send('Runtime.evaluate', {
        expression: 'globalThis.logBrowserResult', awaitPromise: true, returnByValue: true,
      }, sessionId)
      assert.equal(reply.result.value, null, JSON.stringify(reply))
      console.log('Chromium log view: wrapped rows, scrolling, retention and coalesced following passed')
    })()])
  } finally {
    clearTimeout(watchdog)
    proc.kill()
    await closed
  }
}
