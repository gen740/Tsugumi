import { flushSync } from 'react-dom'
import { createRoot } from 'react-dom/client'
import { LiveView, LiveViews, ViewMode } from '../src/model/liveViews'
import { Observable } from '../src/model/observable'
import type { ProjectSession } from '../src/model/session'
import { ProjectCardViews } from '../src/ui/cardViews'
import '../src/styles.css'
import '../src/styles/canvas.css'

const root = createRoot(document.getElementById('root')!)
const project = Object.assign(new Observable(), { liveViews: new LiveViews(), displayName: () => 'A Card', rpc: {
  readCardView: async (name: string, signal: AbortSignal) => {
    if (name === 'waiting') {
      loadingSignal = signal
      return new Promise<string>(() => {})
    }
    return viewHtml(name)
  },
  sendView: async (session: string, to: string, bytes: Uint8Array) => {
    if (to === 'unregistered') throw new Error('No registered recipient')
    replies.push([session, to, [...bytes]])
  },
}, closeLiveView(session: string) {
  if (this.liveViews.close(session)) {
    closed.push(session)
    this.changed()
  }
} })
const closed: string[] = []
const replies: [string, string, number[]][] = []
const broadcast = '*'
const addressed = 'screens/a'
let loadingSignal: AbortSignal | undefined
let openCount = 0
window.open = () => {
  ++openCount
  return null
}

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message)
}
async function until(condition: () => boolean, description: string) {
  for (let attempt = 0; attempt < 250 && !condition(); attempt++)
    await new Promise(resolve => setTimeout(resolve, 20))
  assert(condition(), `Timed out: ${description}\n${JSON.stringify(replies)}`)
}
const equal = (actual: unknown, expected: unknown, message: string) =>
  assert(JSON.stringify(actual) === JSON.stringify(expected), `${message}\n${JSON.stringify(actual)}`)

function viewHtml(session: string) {
  return `<!doctype html><p id="content">empty</p><script>
    const session = ${JSON.stringify(session)}
    const report = (kind, value) => top.postMessage({ viewTest: { session, kind, value } }, '*')
    report('empty', document.getElementById('content').textContent)
    tsugumi.onmessage = bytes => {
      document.getElementById('content').textContent = Array.from(bytes).join(',')
      tsugumi.send(bytes, ${JSON.stringify(addressed)})
    }
    tsugumi.send(Uint8Array.of(12, 0, 255), '*').then(() => report('send-ok'))
    tsugumi.send(Uint8Array.of(9), 'unregistered').catch(error => report('send-rejected', error.message))
    if (session === 'first') {
      addEventListener('securitypolicyviolation', event => {
        if (event.blockedURI.includes('example.invalid')) report('blocked', event.effectiveDirective)
      })
      fetch('https://example.invalid/forbidden').then(() => report('network-open'), () => report('fetch-rejected'))
      const image = new Image()
      image.src = 'https://example.invalid/forbidden.png'
      const frame = document.createElement('iframe')
      frame.src = 'https://example.invalid/forbidden.html'
      document.body.append(frame)
    }
  </script>`
}

type Report = { session: string; kind: string; value: unknown; origin: string }
const reports: Report[] = []
window.addEventListener('message', event => {
  if (event.data?.viewTest) reports.push({ ...event.data.viewTest, origin: event.origin })
})
const render = () => flushSync(() => root.render(<ProjectCardViews session={project as unknown as ProjectSession} />))
function open(session: string, mode: number, type = 'html', width = 0, height = 0) {
  const view = new LiveView(session, 'a', 'view', type, mode, width, height)
  project.liveViews.open(view)
  return view
}

async function test() {
  render()
  assert(!document.querySelector('[role=dialog]'), 'No view before a Card opens one')
  const first = open('first', ViewMode.panel)
  open('second', ViewMode.panel)
  open('bad', ViewMode.panel, 'unsupported')
  const credits: number[] = []
  first.deliver({ bytes: Uint8Array.of(0, 255, 0, 128), complete: () => credits.push(1) })
  first.deliver({ bytes: new Uint8Array(), complete: () => credits.push(2) })
  render()
  await until(() => replies.length === 4, 'panel reverse messages')
  await until(() => reports.some(r => r.kind === 'send-ok') &&
    reports.some(r => r.kind === 'send-rejected' && r.value === 'No registered recipient'), 'HTML send promises settle')
  equal(replies.filter(([session]) => session === 'first'), [
    ['first', broadcast, [12, 0, 255]], ['first', addressed, [0, 255, 0, 128]], ['first', addressed, []],
  ], 'First session gets exact bytes in order')
  equal(replies.filter(([session]) => session === 'second'), [['second', broadcast, [12, 0, 255]]], 'Equal view names remain independent')
  equal(credits, [1, 2], 'The panel releases FIFO credits')
  assert(document.querySelector('[data-view-window="bad"] [role=alert]')?.textContent?.includes('unsupported'), 'Unsupported formats show a diagnostic')
  await until(() => ['connect-src', 'img-src', 'frame-src'].every(d => reports.some(r => r.kind === 'blocked' && r.value === d)), 'sandbox network restrictions')
  assert(reports.filter(r => r.kind === 'empty').every(r => r.origin === 'null' && r.value === 'empty'), 'Opaque frames own their empty state')
  assert(!reports.some(r => r.kind === 'network-open'), 'The frame could not make a network request')

  const frameOf = (session: string) => document.querySelector<HTMLElement>(`[data-view-window="${session}"]`)!
  const buttonOf = (session: string, label: string) =>
    frameOf(session).querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)!
  const loads = (session: string) => reports.filter(r => r.session === session && r.kind === 'empty').length

  // Panels dock beside each other; detaching and attaching only moves the frame.
  assert(frameOf('first').classList.contains('view-window-panel'), 'A panel view docks as a panel')
  const panelFrame = frameOf('first').querySelector('iframe')!
  const panelTop = frameOf('first').getBoundingClientRect().top
  assert(frameOf('second').getBoundingClientRect().top > panelTop, 'Docked panels share the side, one above another')
  buttonOf('first', 'ウィンドウで表示').click()
  render()
  assert(frameOf('first').classList.contains('view-window-window'), 'A panel detaches into a window')
  assert(frameOf('first').querySelector('iframe') === panelFrame, 'Detaching keeps the same frame element')
  first.deliver({ bytes: Uint8Array.of(7) })
  await until(() => replies.some(([session, , bytes]) => session === 'first' && bytes[0] === 7), 'messages after detaching')
  buttonOf('first', 'パネルで表示').click()
  render()
  assert(frameOf('first').classList.contains('view-window-panel'), 'A window attaches back as a panel')
  assert(frameOf('first').querySelector('iframe') === panelFrame, 'Attaching keeps the same frame element')
  first.deliver({ bytes: Uint8Array.of(8) })
  await until(() => replies.some(([session, , bytes]) => session === 'first' && bytes[0] === 8), 'messages after attaching')
  equal(loads('first'), 1, 'The html view never reloads while it moves')

  // WINDOW: an in-page window with the requested content size, no popup.
  const win = open('win', ViewMode.window, 'html', 640, 480)
  win.deliver({ bytes: Uint8Array.of(91), complete: () => credits.push(3) })
  render()
  const windowed = frameOf('win')
  assert(windowed.classList.contains('view-window-window'), 'WINDOW opens an in-page window')
  const body = windowed.querySelector('.float-body')!.getBoundingClientRect()
  // The requested size, kept inside the page (a 12px margin and the 30px title bar).
  equal([body.width, Math.round(body.height)], [Math.min(640, innerWidth - 24), Math.min(480, innerHeight - 24 - 30)],
    'The window content has the requested size')
  await until(() => replies.some(([session, , bytes]) => session === 'win' && bytes[0] === 91), 'window HTML messages')
  equal(replies.filter(([session]) => session === 'win'), [['win', broadcast, [12, 0, 255]], ['win', addressed, [91]]],
    'The window uses the page connection and sends explicit Card paths')
  equal(credits, [1, 2, 3], 'The window returns outstanding credit')

  // Focus follows clicks among several windows.
  const z = (session: string) => Number(frameOf(session).style.zIndex)
  frameOf('first').dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }))
  render()
  assert(z('first') > z('win'), 'A click raises its window')
  windowed.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }))
  render()
  assert(z('win') > z('first'), 'A click on the other window raises that one')

  // Minimize keeps the view running; the strip restores it.
  buttonOf('win', '最小化').click()
  render()
  assert(windowed.classList.contains('minimized') && getComputedStyle(windowed).visibility === 'hidden', 'Minimize hides the window')
  const strip = document.querySelector<HTMLButtonElement>('.view-dock-item')!
  assert(strip?.textContent?.includes('view'), 'The minimized window waits in the strip')
  win.deliver({ bytes: Uint8Array.of(92) })
  await until(() => replies.some(([session, , bytes]) => session === 'win' && bytes[0] === 92), 'a minimized view keeps receiving')
  strip.click()
  render()
  assert(!frameOf('win').classList.contains('minimized') && !document.querySelector('.view-dock-item'), 'The strip restores the window')
  equal(loads('win'), 1, 'Minimize and restore keep the html state')

  // FULLSCREEN fills the page at once; real full screen waits for a click.
  open('full', ViewMode.fullscreen, 'unsupported')
  render()
  const page = frameOf('full')
  let requests = 0
  page.requestFullscreen = async () => { ++requests }
  const filled = page.getBoundingClientRect()
  equal([filled.left, filled.top, filled.width, filled.height], [0, 0, innerWidth, innerHeight], 'FULLSCREEN fills the page')
  assert(!requests, 'Full screen is not requested on open')
  buttonOf('full', 'フルスクリーンにする').click()
  equal(requests, 1, 'The full screen button calls the element API')
  buttonOf('full', 'ウィンドウで表示').click()
  render()
  assert(frameOf('full').classList.contains('view-window-window'), 'A full page view can become a window')
  equal(openCount, 0, 'No view opens a browser popup')

  // Closing the window ends its session.
  buttonOf('win', '閉じる').click()
  render()
  equal(closed, ['win'], 'The close button closes the session once')
  assert(!frameOf('win'), 'The closed window is gone')

  open('waiting', ViewMode.panel)
  render()
  await until(() => !!loadingSignal, 'pending view file read')
  project.closeLiveView('waiting')
  render()
  assert(loadingSignal?.aborted, 'Closing the panel cancels a pending file read')
  flushSync(() => root.unmount())
  assert(!document.querySelector('iframe'), 'Tearing down the project removes frames')
  return null
}

;(globalThis as typeof globalThis & { cardViewsResult: Promise<null | string> }).cardViewsResult =
  test().catch(error => error.stack ?? String(error))
