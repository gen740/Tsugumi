import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { pathToFileURL } from 'node:url'

export async function runInChromium(browser, html, expression) {
  const proc = spawn(browser, [
    '--headless', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking', '--use-mock-keychain',
    `--user-data-dir=${html + '.profile'}`, '--remote-debugging-pipe', 'about:blank',
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
    watchdog = setTimeout(() => reject(new Error(`Chromium test did not finish\n${stderr}`)), 45000)
    proc.once('error', reject)
    proc.once('exit', (code) => reject(new Error(`Chromium exited with ${code}\n${stderr}`)))
    proc.stdio[3].on('error', reject)
  })
  try {
    return await Promise.race([stopped, (async () => {
      const { targetId } = await send('Target.createTarget', { url: 'about:blank' })
      const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true })
      await send('Page.enable', {}, sessionId)
      const loaded = new Promise((resolve) => events.set(`${sessionId}:Page.loadEventFired`, resolve))
      const navigation = await send('Page.navigate', { url: pathToFileURL(html).href }, sessionId)
      assert.equal(navigation.errorText, undefined)
      await loaded
      const reply = await send('Runtime.evaluate', {
        expression, awaitPromise: true, returnByValue: true,
      }, sessionId)
      assert.equal(reply.exceptionDetails, undefined, JSON.stringify(reply))
      return reply.result.value
    })()])
  } finally {
    clearTimeout(watchdog)
    proc.kill()
    await closed
  }
}
