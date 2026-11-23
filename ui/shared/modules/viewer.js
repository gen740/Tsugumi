// Trusted bridge between the browser UI and the sandboxed Card document.
'use strict'
const policy = document.querySelector('meta[http-equiv="Content-Security-Policy"]').content
const messageMax = 1 << 24
const budget = 2 * messageMax

// Acknowledges a delivered MessagePort event after the view's handlers ran.
// The Card's data messages keep the public {type, bytes} shape.
const creditScript = `<script>
addEventListener('message', event => {
  if (event.source !== parent || event.data?.tsugumi !== 1) return
  const port = event.data.port
  const max = 1 << 24
  const post = port.postMessage.bind(port)
  let used = 0, reportedDrop = false
  port.postMessage = (message, options) => {
    if (message?.type !== 'data' || !(message.bytes instanceof ArrayBuffer)) return
    const size = message.bytes.byteLength
    if (size > max || used > max) {
      if (!reportedDrop) post({ type: 'dropped' })
      reportedDrop = true
      return
    }
    post(message, options)
    used += Math.max(1, size)
  }
  port.addEventListener('message', message => {
    if (message.data?.type === 'reverseCredit') {
      message.stopImmediatePropagation()
      used -= Math.max(1, message.data.size)
      reportedDrop = false
    } else if (message.data?.type === 'data') {
      setTimeout(() => post({ type: 'credit' }), 0)
    }
  })
  port.start()
}, { once: true })
<\/script>`

let current

function notify(message, transfer = []) {
  if (parent !== window) parent.postMessage(message, '*', transfer)
}

function dispose() {
  if (!current) return
  current.port?.close()
  current.frame.remove()
  current = undefined
}

function flush(view) {
  while (view.port && view.pending.length && view.outBytes <= budget - messageMax) {
    const { bytes, ticket } = view.pending.shift()
    const size = Math.max(1, bytes.byteLength)
    view.outBytes += size
    view.out.push(size)
    view.port.postMessage({ type: 'data', bytes }, [bytes])
    notify({ tsugumi: 'credit', ticket })
  }
}

function mount(html) {
  dispose()
  const frame = document.createElement('iframe')
  frame.setAttribute('sandbox', 'allow-scripts')
  frame.title = 'Card view'
  const content = html.replace(/^\s*<!doctype\s+html\s*>/i, '')
  frame.srcdoc = `<!doctype html><meta http-equiv="Content-Security-Policy" content="${policy}">${creditScript}${content}`
  const view = current = { frame, pending: [], out: [], outBytes: 0, reverseBytes: 0 }
  frame.addEventListener('load', () => {
    if (current !== view) return
    const { port1, port2 } = new MessageChannel()
    view.port = port1
    port1.onmessage = ({ data }) => {
      if (current !== view) return
      if (data?.type === 'credit') {
        const sent = view.out.shift()
        if (!sent) return
        view.outBytes -= sent
        flush(view)
      } else if (data?.type === 'dropped') {
        notify({ tsugumi: 'messageDropped' })
      } else if (data?.type === 'data' && data.bytes instanceof ArrayBuffer) {
        if (data.bytes.byteLength > messageMax || view.reverseBytes > budget - messageMax) {
          notify({ tsugumi: 'messageDropped' })
          view.port.postMessage({ type: 'reverseCredit', size: data.bytes.byteLength })
          return
        }
        view.reverseBytes += Math.max(1, data.bytes.byteLength)
        notify({ tsugumi: 'send', bytes: data.bytes }, [data.bytes])
      }
    }
    frame.contentWindow.postMessage({ tsugumi: 1, port: port2 }, '*', [port2])
    flush(view)
  }, { once: true })
  document.body.append(frame)
}

window.tsugumiView = message => {
  switch (message?.type) {
    case 'mount':
      if (typeof message.html === 'string') mount(message.html)
      return
    case 'dispose':
      dispose()
      return
    case 'data':
      if (!current || !(message.bytes instanceof ArrayBuffer)) return
      current.pending.push({ bytes: message.bytes, ticket: message.ticket })
      flush(current)
      return
    case 'reverseCredit':
      if (current && Number.isInteger(message.size) && message.size >= 0) {
        current.reverseBytes = Math.max(0, current.reverseBytes - Math.max(1, message.size))
        current.port?.postMessage({ type: 'reverseCredit', size: message.size })
      }
  }
}

addEventListener('message', event => {
  if (parent === window || event.source !== parent) return
  if (event.data?.tsugumi === 'probe') notify({ tsugumi: 'ready' })
  if (event.data?.tsugumi === 'view') window.tsugumiView(event.data.message)
})
addEventListener('pagehide', dispose)
notify({ tsugumi: 'ready' })
