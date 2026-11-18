// Trusted transport only. Card code runs in a second, opaque-origin frame.
// Both WKWebView and the browser use this exact host and module lifecycle.
'use strict'
const frame = document.createElement('iframe')
frame.sandbox = 'allow-scripts'
frame.allow = 'webgpu'
frame.title = 'Card view'
const policy = "default-src 'none'; script-src blob: 'unsafe-inline' 'unsafe-eval'; style-src 'unsafe-inline'; img-src blob: data:; font-src data:; connect-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'"
function runtime() {
  const root = document.createElement('div')
  Object.assign(root.style, { position: 'absolute', inset: '0', overflow: 'auto' })
  document.body.append(root)
  let module, identity, currentData, viewError, initialised = false, visible = true, colorScheme = 'light', pending, working = false, resizePending = false
  const size = () => ({ width: innerWidth, height: innerHeight, pixelRatio: devicePixelRatio, visible, colorScheme })
  const send = data => parent.postMessage({ tsugumi: 'state', ...data }, '*')
  root.addEventListener('tsugumi-view-error', event => {
    const message = String(event.detail).slice(0, 4096)
    viewError = new Error(message)
    send({ state: 'error', message })
    if (!working) {
      identity = undefined; initialised = false
      void module?.dispose()
      root.textContent = message
    }
  })
  async function render(message) {
    try {
      viewError = undefined
      visible = message.visible !== false
      colorScheme = message.colorScheme === 'dark' ? 'dark' : 'light'
      if (identity !== message.name) {
        await module?.dispose()
        root.replaceChildren()
        module = undefined; initialised = false
        const url = URL.createObjectURL(new Blob([message.code], { type: 'text/javascript' }))
        try { module = await import(url) } finally { URL.revokeObjectURL(url) }
        for (const name of ['initialise', 'update', 'resize', 'dispose'])
          if (typeof module[name] !== 'function') throw new Error(`View must export ${name}`)
        identity = message.name
        currentData = { config: JSON.parse(message.json), blobs: message.blobs ?? {} }
        await module.initialise(root, currentData, size())
        initialised = true
      } else {
        currentData = { config: JSON.parse(message.json), blobs: message.blobs ?? {} }
        await module.update(currentData)
        await module.resize(size())
      }
      await new Promise(resolve => setTimeout(resolve, 30))
      if (viewError) throw viewError
      let image
      if (message.snapshot) {
        if (module.snapshot) image = await module.snapshot()
        else {
          const canvas = root.querySelector('canvas')
          if (canvas) image = canvas.toDataURL('image/png')
          else {
            // DOM/SVG views can export a PNG through snapshot().
            throw new Error('A DOM view must export snapshot() for canvas previews')
          }
        }
      }
      send({ state: 'ok', requestId: message.requestId, image })
    } catch (e) {
      identity = undefined; initialised = false
      try { await module?.dispose() } catch { /* Keep the original diagnostic. */ }
      root.textContent = String(e?.message ?? e)
      send({ state: 'error', requestId: message.requestId, message: root.textContent })
    }
  }
  async function drain() {
    if (working) return
    working = true
    try {
      while (pending || resizePending) {
        if (pending) { const next = pending; pending = undefined; await render(next) }
        else {
          resizePending = false
          if (initialised) await module.resize(size())
        }
      }
    } catch (e) { send({ state: 'error', message: String(e?.message ?? e) }) }
    finally { working = false }
  }
  addEventListener('message', async event => {
    if (event.source !== parent) return
    if (event.data?.tsugumi === 'visibility') {
      visible = event.data.visible; resizePending = true; await drain(); return
    }
    if (event.data?.tsugumi !== 'render') return
    if (pending) send({ state: 'superseded', requestId: pending.requestId })
    pending = event.data
    await drain()
  })
  new ResizeObserver(() => { resizePending = true; void drain() }).observe(root)
  addEventListener('pagehide', () => { void module?.dispose() })
  addEventListener('keydown', e => { if (e.key === 'Escape') parent.postMessage({ tsugumi: 'key', key: 'Escape' }, '*') })
  parent.postMessage({ tsugumi: 'inner-ready' }, '*')
}
frame.srcdoc = `<meta http-equiv="Content-Security-Policy" content="${policy}"><style>html,body{margin:0;height:100%;overflow:hidden}</style><script>(${runtime.toString()})()<` + '/script>'
let readyResolve
const ready = new Promise(resolve => { readyResolve = resolve })
let sequence = 0
let intersecting = true, preview = false
const visibility = () => intersecting && document.visibilityState !== 'hidden'
function updateVisibility() {
  frame.contentWindow?.postMessage({ tsugumi: 'visibility', visible: preview || visibility() }, '*')
}
new IntersectionObserver(entries => { intersecting = entries.some(entry => entry.isIntersecting); updateVisibility() }).observe(frame)
addEventListener('visibilitychange', updateVisibility)
const waiting = new Map()
function notify(message) {
  if (parent !== window) parent.postMessage(message, '*')
  window.webkit?.messageHandlers?.tsugumi?.postMessage(message)
}
window.tsugumiRender = async message => {
  await ready
  preview = message.interactive === false
  const requestId = message.requestId ?? ++sequence
  const blobs = Object.fromEntries(Object.entries(message.blobs ?? {}).map(([name, value]) => [name,
    typeof value === 'string' ? Uint8Array.from(atob(value), c => c.charCodeAt(0)) : value.slice()]))
  const promise = new Promise(resolve => waiting.set(requestId, resolve))
  frame.contentWindow.postMessage({ ...message, blobs, visible: message.interactive === false || (message.visible !== false && visibility()), tsugumi: 'render', requestId }, '*', Object.values(blobs).map(b => b.buffer))
  return await promise
}
addEventListener('message', event => {
  if (event.source === frame.contentWindow) {
    const message = event.data
    if (message?.tsugumi === 'inner-ready') { readyResolve(); notify({ tsugumi: 'ready' }); return }
    if (message?.tsugumi === 'state') {
      document.body.dataset.state = message.state
      document.body.dataset.error = String(message.message ?? '')
      waiting.get(message.requestId)?.(message.state === 'ok'); waiting.delete(message.requestId)
      if (message.state !== 'superseded') notify({ tsugumi: 'state', state: message.state, requestId: message.requestId, image: message.image, message: String(message.message ?? '').slice(0, 4096) })
    }
    if (message?.tsugumi === 'key') notify({ tsugumi: 'key', key: 'Escape' })
  } else if (event.source === parent && event.data?.name) {
    void window.tsugumiRender(event.data)
  }
})
addEventListener('pagehide', () => frame.remove())
document.body.append(frame)
