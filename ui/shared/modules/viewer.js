// Trusted bridge shared by the browser and the Mac's WKWebView.
'use strict'
const policy = document.querySelector('meta[http-equiv="Content-Security-Policy"]').content

let current

function notify(message) {
  if (parent !== window) parent.postMessage(message, '*')
  window.webkit?.messageHandlers?.tsugumi?.postMessage(message)
}

function dispose() {
  if (!current) return
  current.port?.close()
  current.frame.remove()
  current = undefined
}

function send(message) {
  if (message.type === 'data') current.port.postMessage(message, [message.bytes])
  else current.port.postMessage(message)
}

function mount(html) {
  dispose()
  const frame = document.createElement('iframe')
  frame.setAttribute('sandbox', 'allow-scripts')
  frame.title = 'Card view'
  // Use standards mode and enforce the policy before any of the Card's markup.
  const content = html.replace(/^\s*<!doctype\s+html\s*>/i, '')
  frame.srcdoc = `<!doctype html><meta http-equiv="Content-Security-Policy" content="${policy}">${content}`
  const view = current = { frame, pending: [] }
  frame.addEventListener('load', () => {
    if (current !== view) return
    const { port1, port2 } = new MessageChannel()
    view.port = port1
    frame.contentWindow.postMessage({ tsugumi: 1, port: port2 }, '*', [port2])
    for (const message of view.pending) send(message)
    view.pending = []
  }, { once: true })
  document.body.append(frame)
}

// Mount with {type: "mount", html}. The Mac may pass data bytes as base64.
// Run and data messages on the Card's MessagePort use the ABI shapes.
window.tsugumiView = message => {
  switch (message?.type) {
    case 'mount':
      if (typeof message.html === 'string') mount(message.html)
      return
    case 'dispose':
      dispose()
      return
    case 'data': {
      if (!current) return
      const bytes = typeof message.bytes === 'string'
        ? Uint8Array.from(atob(message.bytes), c => c.charCodeAt(0)).buffer
        : message.bytes
      if (bytes instanceof ArrayBuffer) enqueue({ type: 'data', bytes })
      return
    }
    case 'run':
      if (current && (message.state === 'started' || message.state === 'ended'))
        enqueue({ type: 'run', state: message.state })
  }
}

function enqueue(message) {
  if (current.port) send(message)
  else if (message.type === 'data' && current.pending.at(-1)?.type === 'data')
    current.pending[current.pending.length - 1] = message
  else current.pending.push(message)
}

addEventListener('message', event => {
  if (parent === window || event.source !== parent) return
  if (event.data?.tsugumi === 'probe') notify({ tsugumi: 'ready' })
  if (event.data?.tsugumi === 'view') window.tsugumiView(event.data.message)
})
addEventListener('pagehide', dispose)
notify({ tsugumi: 'ready' })
