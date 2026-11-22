import { flushSync } from 'react-dom'
import { createRoot } from 'react-dom/client'
import { CardLiveViews, type LiveView } from '../src/model/liveViews'
import { CardViewsPanel } from '../src/ui/cardViews'

const root = createRoot(document.getElementById('root')!)
const card = new CardLiveViews()
card.declare([
  { name: 'first', type: 'html', blob: '/nix/store/first' },
  { name: 'second', type: 'html', blob: '/nix/store/second' },
  { name: 'gpu', type: 'dawn', blob: '/nix/store/gpu' },
  { name: 'bad', type: 'unexpected-type', blob: '/nix/store/bad' },
  { name: 'twin', type: 'dawn', blob: '/nix/store/twin-a' },
  { name: 'twin', type: 'dawn', blob: '/nix/store/twin-b' },
])
card.data('missing', Uint8Array.of(1))

type Report = { name: string; kind: string; value: unknown; origin: string }
const reports: Report[] = []
window.addEventListener('message', event => {
  if (event.data?.viewTest) reports.push({ ...event.data.viewTest, origin: event.origin })
})
function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(`${message}\n${JSON.stringify(reports)}`)
}
async function until(condition: () => boolean) {
  for (let attempt = 0; attempt < 250 && !condition(); attempt++)
    await new Promise(resolve => setTimeout(resolve, 20))
  assert(condition(), 'Timed out waiting for the view')
}

const requested: string[] = []
let resolveSecond!: (html: string) => void
const secondSource = new Promise<string>(resolve => { resolveSecond = resolve })
let loadingSignal: AbortSignal | undefined
const read = async (view: LiveView, signal: AbortSignal): Promise<string> => {
  const name = view.declaration.name
  requested.push(name)
  if (name === 'waiting') {
    loadingSignal = signal
    return new Promise(() => {})
  }
  return name === 'second' ? secondSource : viewHtml(name)
}
function viewHtml(name: string) {
  return `<!doctype html><html><head><title>${name}</title></head><body><p id="content">empty</p><script>
    const name = ${JSON.stringify(name)}
    const report = (kind, value) => top.postMessage({ viewTest: { name, kind, value } }, '*')
    const policy = new Set()
    addEventListener('securitypolicyviolation', event => {
      if (event.blockedURI.includes('example.invalid')) {
        policy.add(event.effectiveDirective)
        report('blocked', event.effectiveDirective)
      }
    })
    addEventListener('message', event => {
      if (event.source !== parent || event.data?.tsugumi !== 1) return
      const { port } = event.data
      const element = document.getElementById('content')
      report('empty', element.textContent)
      port.onmessage = ({ data }) => {
        if (data.type === 'run') report('run', data.state)
        else {
          if (!(data.bytes instanceof ArrayBuffer)) return report('wrong-type', typeof data.bytes)
          const bytes = Array.from(new Uint8Array(data.bytes))
          element.textContent = bytes.join(',')
          report('data', bytes)
        }
      }
      port.start()
      if (name === 'first') {
        fetch('https://example.invalid/forbidden').then(() => report('network-open'), () => report('fetch-rejected'))
        const image = new Image()
        image.src = 'https://example.invalid/forbidden.png'
        const subframe = document.createElement('iframe')
        subframe.src = 'https://example.invalid/forbidden.html'
        document.body.append(subframe)
      }
    })
  </script></body></html>`
}

const have = (kind: string, view?: string) => reports.some(r => r.kind === kind && (!view || r.name === view))
const traffic = (name: string) => reports.filter(r => r.name === name && ['data', 'run'].includes(r.kind)).map(r => [r.kind, r.value])
const equal = (actual: unknown, expected: unknown, message: string) => assert(JSON.stringify(actual) === JSON.stringify(expected), message)
const render = () => flushSync(() => root.render(<CardViewsPanel card={card} read={read} />))

async function test() {
  render()
  await until(() => have('empty', 'first') && have('fetch-rejected'))
  equal(traffic('first'), [], 'An idle frame starts empty')
  assert(reports.find(r => r.kind === 'empty')?.value === 'empty', 'The Card owns its empty state')
  equal(requested, ['first', 'second'], 'The UI reads only declared HTML views')
  assert(document.body.textContent?.includes('Dawn'), 'Dawn shows a placeholder')
  const errors = [...document.querySelectorAll('[role=alert]')].map(e => e.textContent)
  assert(errors.some(t => t?.includes('unexpected-type')) && errors.some(t => t?.includes('missing')), 'Both view errors are visible')
  assert(errors.some(t => t?.includes('twin')), 'A repeated view name is reported')
  assert(reports.filter(r => r.kind === 'empty').every(r => r.origin === 'null'), 'View frames have opaque origins')
  await until(() => ['connect-src', 'img-src', 'frame-src'].every(d => reports.some(r => r.kind === 'blocked' && r.value === d)))
  assert(!have('network-open'), 'The view could not make a network request')

  card.run('r', 'started')
  card.data('first', Uint8Array.of(99))
  card.data('first', Uint8Array.of(0, 255, 0, 128))
  card.data('second', Uint8Array.of(8, 0, 9))
  card.run('r', 'ended')
  resolveSecond(viewHtml('second'))
  await until(() => traffic('first').length === 3 && traffic('second').length === 3)
  equal(traffic('first'), [['run', 'started'], ['data', [0, 255, 0, 128]], ['run', 'ended']], 'First view routes its latest bytes between controls')
  equal(traffic('second'), [['run', 'started'], ['data', [8, 0, 9]], ['run', 'ended']], 'Control order survives a slow view load')
  card.data('second', new Uint8Array())
  await until(() => traffic('second').length === 4)
  equal(traffic('second').at(-1), ['data', []], 'A post-run empty message still arrives')
  equal(traffic('first').length, 3, 'Post-run data stays with its addressed view')

  const first = document.querySelector<HTMLIFrameElement>('iframe[title=first]')!
  card.declare([{ name: 'waiting', type: 'html', blob: '/nix/store/waiting' }])
  render()
  await until(() => loadingSignal !== undefined)
  assert(!first.isConnected, 'Declaration changes detach the old frame')
  flushSync(() => root.unmount())
  assert(!document.querySelector('iframe'), 'Closing the panel removes every frame')
  assert(loadingSignal?.aborted, 'A pending blob read is cancelled on disposal')
  return null
}

;(globalThis as typeof globalThis & { cardViewsResult: Promise<null | string> }).cardViewsResult =
  test().catch(error => error.stack ?? String(error))
