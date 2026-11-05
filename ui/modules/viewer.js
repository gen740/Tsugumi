// The module viewer: draws {name, json, interactive} sent by the web UI
// (postMessage from the parent page) or by the app (tsugumiRender(...)).
//   plotly     a Plotly figure's JSON ({data, layout}) with plotly.js
//   vega-lite  a Vega-Lite spec with Vega / Vega-Lite / vega-embed
// A Card's plot is untrusted: nothing here loads from the network (Vega's
// loader refuses every URL; the page's policy allows no connections either).
// In the web UI this page is sandboxed (an opaque origin, which loads
// nothing): the parent page sends the libraries' code. In the app the
// libraries are files next to this page.
'use strict'

const framed = window.parent !== window
const loaded = new Map()
const asked = new Map()
function load(src) {
  if (!loaded.has(src)) {
    loaded.set(src, framed
      ? new Promise((resolve, reject) => {
        asked.set(src, { resolve, reject })
        window.parent.postMessage({ tsugumi: 'need', lib: src }, '*')
      })
      : new Promise((resolve, reject) => {
        const s = document.createElement('script')
        s.src = src
        s.onload = resolve
        s.onerror = () => reject(new Error(`cannot load ${src}`))
        document.head.appendChild(s)
      }))
  }
  return loaded.get(src)
}

const view = document.getElementById('view')
const error = document.getElementById('error')
let drawn = ''   // the module drawn now
let current = 0  // the latest request (a slower earlier one does not draw over it)

function done(state, message) {
  document.body.dataset.state = state
  error.textContent = message ?? ''
  const msg = { tsugumi: 'state', state, message: message ?? '' }
  if (window.parent !== window) window.parent.postMessage(msg, '*')
  window.webkit?.messageHandlers?.tsugumi?.postMessage(msg)
}

async function plotly(spec, interactive) {
  await load('plotly.min.js')
  // The display frame decides the size.
  const layout = { ...(spec.layout ?? {}), autosize: true }
  delete layout.width
  delete layout.height
  await window.Plotly.react(view, spec.data ?? [], layout, {
    responsive: true, staticPlot: !interactive, displaylogo: false, displayModeBar: interactive ? 'hover' : false,
  })
}

// Not named vegaLite: the library sets that global, and a second chart on the page would call it.
async function drawVegaLite(spec, interactive) {
  for (const s of ['vega.min.js', 'vega-lite.min.js', 'vega-embed.min.js']) await load(s)
  const composite = ['hconcat', 'vconcat', 'concat', 'facet', 'repeat'].some((k) => k in spec)
  if (!composite) {
    // A single view fills the frame; a composite keeps its size (and scrolls when interactive).
    spec.width = 'container'
    spec.height = 'container'
    spec.autosize = { type: 'fit', contains: 'padding' }
  }
  document.body.classList.toggle('scroll', composite && interactive)
  const loader = window.vega.loader()
  loader.load = loader.sanitize = async (uri) => { throw new Error(`a Card's chart cannot load ${uri}: put its data in the spec`) }
  view.replaceChildren()
  await window.vegaEmbed(view, spec, {
    actions: interactive ? { export: true, source: false, compiled: false, editor: false } : false,
    loader, renderer: 'canvas', tooltip: interactive,
  })
}

async function render({ name, json, interactive }) {
  const n = ++current
  try {
    const spec = JSON.parse(json)
    document.body.classList.toggle('still', !interactive)
    if (drawn === 'plotly' && name !== 'plotly') window.Plotly?.purge(view)
    if (name === 'plotly') await plotly(spec, interactive)
    else if (name === 'vega-lite') await drawVegaLite(spec, interactive)
    else throw new Error(`no viewer for the module ${name}`)
    drawn = name
    if (n === current) done('ok')
  } catch (e) {
    if (n === current) done('error', String(e?.message ?? e))
  }
}

// The app calls this; the web UI posts {name, json, interactive}.
window.tsugumiRender = render
window.addEventListener('message', (e) => {
  if (e.source !== window.parent || !e.data) return
  if (e.data.tsugumi === 'lib' && asked.has(e.data.lib)) {
    const { resolve, reject } = asked.get(e.data.lib)
    asked.delete(e.data.lib)
    if (typeof e.data.code !== 'string') return reject(new Error(`cannot load ${e.data.lib}`))
    try {
      (0, eval)(e.data.code) // global scope: the library defines its global (Plotly, vega, ...)
      resolve()
    } catch (err) {
      reject(err)
    }
  } else if (typeof e.data.json === 'string') render(e.data)
})
// Esc belongs to the page (it closes the expanded view), also while the plot has the focus.
window.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && framed) window.parent.postMessage({ tsugumi: 'key', key: 'Escape' }, '*')
})
if (framed) window.parent.postMessage({ tsugumi: 'ready' }, '*')
window.webkit?.messageHandlers?.tsugumi?.postMessage({ tsugumi: 'ready' })
