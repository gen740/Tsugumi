import { createRoot } from 'react-dom/client'
import { LiveView } from '../src/model/liveViews'
import { DawnViewCanvas } from '../src/ui/dawnViewCanvas'
import { encodeView, i32, i64 } from './dawnFixture'

const constant = (value: number) => [0x41, value]
const mark = (value: number) => [
  ...constant(0), ...constant(value), 0x3a, 0, 0,
  ...constant(16), ...constant(42), 0x3a, 0, 0,
  ...constant(16), ...constant(0), ...constant(1), 0x10, 0, 0x1a,
]
const bytes = encodeView({
  imports: [{ module: 'tsugumi', name: 'tsugumi_view_send', params: [i32, i32, i32] }],
  callbacks: {
    draw: { params: [i32, i64], code: [...mark(1), ...constant(0)] },
    key: { params: [i32, i32, i32, i64], code: [
      ...constant(0), 0x20, 1, ...constant(44), 0x46, 0x04, 0x40,
      // while (true) {} on the view's Worker.
      0x03, 0x40, 0x0c, 0, 0x0b, 0x0b,
    ] },
    text_input: { code: constant(32) },
    text: { params: [i32, i64], code: [...mark(3), ...constant(0)] },
    composition: { params: [i32, i32, i64], code: [...mark(4), ...constant(0)] },
    audio: { params: [i32, i64], code: [
      // Return silence. Mark the first request from the AudioWorklet.
      ...constant(1), 0x2d, 0, 0, 0x45, 0x04, 0x40,
      ...constant(1), ...constant(1), 0x3a, 0, 0, ...mark(2), 0x0b, ...constant(0),
    ] },
  },
})

function requireValue<T>(value: T | null | undefined, description: string): T {
  if (!value) throw new Error(description)
  return value
}

async function until(condition: () => unknown, description: string) {
  for (let attempt = 0; attempt < 300; ++attempt) {
    if (condition()) return
    await new Promise(resolve => setTimeout(resolve, 20))
  }
  throw new Error(`Timed out: ${description}`)
}

async function test() {
  const doc = document
  const host = doc.createElement('main')
  host.style.cssText = 'width: 320px; height: 240px; display: flex; flex-direction: column'
  doc.body.append(host)
  const root = createRoot(host)
  const view = new LiveView('dawn', 'card', 'dawn', 'dawn', 1, 320, 240)
  view.setRecipient('card', true)
  const messages: number[] = []
  const show = (hidden: boolean) => root.render(<DawnViewCanvas view={view} read={async () => bytes.slice()}
    send={async (_, data, to) => {
      if (to !== '*') throw new Error('Unexpected Dawn address')
      messages.push(data[0])
    }} hidden={hidden} />)
  const draws = () => messages.filter(value => value === 1).length
  try {
    show(false)
    await until(() => messages.includes(1), 'the canvas drew in the Worker')

    // Minimized: messages still reach the view, but it is not drawn until restored.
    show(true)
    await new Promise(resolve => setTimeout(resolve, 100))
    const shown = draws()
    view.deliver({ bytes: Uint8Array.of(1) })
    await new Promise(resolve => setTimeout(resolve, 300))
    if (draws() !== shown) throw new Error('A hidden view must not be drawn')
    show(false)
    await until(() => draws() > shown, 'restoring the view drew it again')
    await until(() => doc.querySelector('textarea:not([hidden])'), 'the IME textarea follows the view text area')
    const textarea = requireValue(doc.querySelector('textarea'), 'The textarea is missing')
    textarea.dispatchEvent(new CompositionEvent('compositionstart'))
    textarea.dispatchEvent(new CompositionEvent('compositionupdate', { data: 'にほんご' }))
    textarea.dispatchEvent(new CompositionEvent('compositionend', { data: '日本語' }))
    await until(() => messages.includes(3) && messages.includes(4), 'committed and composing text reached the Worker')
    await until(() => messages.includes(2), 'the AudioWorklet requested a buffer')

    const image = await createImageBitmap(new Blob([await view.snapshot()], { type: 'image/png' }))
    if (!image.width || !image.height) throw new Error('The offscreen PNG is empty')
    image.close()

    const stuck = view.sendInput({ type: 'key', kind: 0, code: 44, modifiers: 0 }).then(() => false, () => true)
    await until(() => doc.querySelector('.dawn-view-waiting'), 'a stuck export showed the waiting overlay')
    const duringStop = view.snapshot().then(() => false, () => true)
    requireValue(doc.querySelector<HTMLButtonElement>('.dawn-view-waiting button'), 'The Stop button is missing').click()
    if (!await stuck || !await duringStop) throw new Error('Stop must reject queued renderer requests')
    await until(() => !doc.querySelector('.dawn-view-waiting'), 'Stop cleared the waiting overlay')
  } catch (error) {
    throw new Error(`${error instanceof Error ? error.message : error}\n${doc.body.innerText}`)
  } finally {
    view.close()
    root.unmount()
    host.remove()
  }
}

;(globalThis as typeof globalThis & { dawnWorkerResult: Promise<string | null> }).dawnWorkerResult =
  test().then(() => null, error => error.stack ?? String(error))
