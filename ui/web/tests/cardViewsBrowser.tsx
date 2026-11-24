import { flushSync } from 'react-dom'
import { createRoot } from 'react-dom/client'
import { LiveView, LiveViews, ViewMode } from '../src/model/liveViews'
import { Observable } from '../src/model/observable'
import type { ProjectSession } from '../src/model/session'
import { ProjectCardViews } from '../src/ui/cardViews'

const root = createRoot(document.getElementById('root')!)
const project = Object.assign(new Observable(), { liveViews: new LiveViews(), displayName: () => 'A Card', rpc: {
  readCardView: async (name: string, signal: AbortSignal) => {
    if (name === 'waiting') {
      loadingSignal = signal
      return new Promise<string>(() => {})
    }
    return viewHtml(name)
  },
  sendView: async (session: string, route: Uint8Array, bytes: Uint8Array) => { replies.push([session, [...route], [...bytes]]) },
}, closeLiveView(session: string) {
  if (this.liveViews.close(session)) {
    closed.push(session)
    this.changed()
  }
} })
const closed: string[] = []
const replies: [string, number[], number[]][] = []
const opener = Array(16).fill(0)
const addressed = Array.from({ length: 16 }, (_, i) => 0xf0 + i)
let loadingSignal: AbortSignal | undefined
const nativeOpen = window.open.bind(window)
let blocked = false
let child: Window | null | undefined
let features: string | undefined
let openCount = 0
window.open = (url, target, flags) => {
  ++openCount
  features = flags
  return child = blocked ? null : nativeOpen(url, target, flags)
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
      tsugumi.send(bytes, Uint8Array.from(${JSON.stringify(addressed)}))
    }
    tsugumi.send(Uint8Array.of(12, 0, 255))
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
  equal(replies.filter(([session]) => session === 'first'), [
    ['first', opener, [12, 0, 255]], ['first', addressed, [0, 255, 0, 128]], ['first', addressed, []],
  ], 'First session gets exact bytes in order')
  equal(replies.filter(([session]) => session === 'second'), [['second', opener, [12, 0, 255]]], 'Equal view names remain independent')
  equal(credits, [1, 2], 'The panel releases FIFO credits')
  assert(document.querySelector('[role=alert]')?.textContent?.includes('unsupported'), 'Unsupported formats show a diagnostic')
  await until(() => ['connect-src', 'img-src', 'frame-src'].every(d => reports.some(r => r.kind === 'blocked' && r.value === d)), 'sandbox network restrictions')
  assert(reports.filter(r => r.kind === 'empty').every(r => r.origin === 'null' && r.value === 'empty'), 'Opaque frames own their empty state')
  assert(!reports.some(r => r.kind === 'network-open'), 'The frame could not make a network request')

  blocked = true
  const popup = open('popup', ViewMode.window, 'html', 640, 480)
  popup.deliver({ bytes: Uint8Array.of(91), complete: () => credits.push(3) })
  render()
  await until(() => !!document.querySelector('[role=status]'), 'popup blocker fallback')
  equal(credits, [1, 2], 'A blocked popup holds its first message')
  const retry = [...document.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === '別ウィンドウで表示')!
  blocked = false
  retry.click()
  await until(() => !!child && replies.some(([session, , bytes]) => session === 'popup' && bytes[0] === 91), 'child HTML messages')
  assert(child?.document.querySelector('iframe'), 'The HTML sandbox runs inside the child window')
  equal(features, 'popup,width=640,height=480', 'Specified content size reaches the browser')
  equal(replies.filter(([session]) => session === 'popup'), [
    ['popup', opener, [12, 0, 255]], ['popup', addressed, [91]],
  ], 'The child uses the opener connection and sends explicit routes')
  equal(credits, [1, 2, 3], 'The child returns outstanding credit')
  child.close()
  await until(() => closed.includes('popup'), 'close button or browser close sends the same project close')
  equal(closed, ['popup'], 'The child closes exactly once')
  equal(openCount, 2, 'One automatic attempt and one click retry')

  open('full', ViewMode.fullscreen, 'unsupported')
  render()
  await until(() => !!child?.document.querySelector('.card-view-fullscreen button'), 'fullscreen requires a child button')
  const fullDoc = child!.document
  let requests = 0
  fullDoc.querySelector('main')!.requestFullscreen = async () => { ++requests }
  assert(!requests, 'Fullscreen is not requested on open')
  fullDoc.querySelector<HTMLButtonElement>('.card-view-fullscreen button')!.click()
  equal(requests, 1, 'The fullscreen button calls the child element API')

  open('waiting', ViewMode.panel)
  render()
  await until(() => !!loadingSignal, 'pending view file read')
  project.closeLiveView('waiting')
  render()
  assert(loadingSignal?.aborted, 'Closing the panel cancels a pending file read')
  flushSync(() => root.unmount())
  assert(!document.querySelector('iframe'), 'Tearing down the project removes frames')
  assert(child?.closed, 'Tearing down the project closes its child windows')
  return null
}

;(globalThis as typeof globalThis & { cardViewsResult: Promise<null | string> }).cardViewsResult =
  test().catch(error => error.stack ?? String(error))
