import { keyUsage, modifiers, viewKeysAttribute, wheelPixels } from './input'
import { viewFlags, type DawnInput, type TextArea, type ViewFeatures } from './workerProtocol'

const encoder = new TextEncoder()

export function observeInput(
  canvas: HTMLCanvasElement,
  textarea: HTMLTextAreaElement,
  flags: number,
  features: ViewFeatures,
  input: (event: DawnInput, at: number) => void,
) {
  const events = new AbortController()
  const { signal } = events
  let textArea: TextArea | null = null
  let composing = false
  let composition = ''
  let compositionCursor = 0
  let committed: string | undefined
  let commitTimer: number | undefined
  let gamepadFrame: number | undefined

  const at = (event: Event) => performance.timeOrigin + event.timeStamp
  const updateComposition = (text: string, timestamp: number) => {
    const offset = textarea.value === text ? textarea.selectionStart : text.length
    const cursor = encoder.encode(text.slice(0, offset)).length
    if (text === composition && cursor === compositionCursor) return
    composition = text
    compositionCursor = cursor
    if (features.composition) input({ type: 'composition', text, cursor }, timestamp)
  }

  canvas.addEventListener('pointerdown', event => {
    // Pointer lock requires a user gesture, and Esc is always owned by the browser while locked.
    if (event.button === 0 && (flags & viewFlags.lockPointer) && document.pointerLockElement !== canvas) {
      try {
        void Promise.resolve(canvas.requestPointerLock()).catch(() => {})
      } catch { /* a browser may reject lock here while leaving ordinary input enabled */ }
    }
    if (textArea) {
      event.preventDefault()
      textarea.focus({ preventScroll: true })
    }
  }, { signal })

  if (features.pointer) {
    const pointer = (event: MouseEvent, kind: number, wheelX = 0, wheelY = 0) => {
      const rect = canvas.getBoundingClientRect()
      const locked = document.pointerLockElement === canvas
      const x = locked ? event.movementX : event.clientX - rect.left
      const y = locked ? event.movementY : event.clientY - rect.top
      input({ type: 'pointer', kind, x, y, buttons: event.buttons & 7, wheelX, wheelY, modifiers: modifiers(event) }, at(event))
    }
    canvas.addEventListener('pointermove', event => pointer(event, 0), { signal })
    canvas.addEventListener('pointerdown', event => pointer(event, 1), { signal })
    canvas.addEventListener('pointerup', event => pointer(event, 2), { signal })
    canvas.addEventListener('wheel', event => {
      event.preventDefault()
      pointer(event, 3, ...wheelPixels(event, canvas))
    }, { signal, passive: false })
    canvas.addEventListener('pointerleave', event => pointer(event, 4), { signal })
  }

  if (features.key) {
    const key = (event: KeyboardEvent, kind: number) => {
      const code = keyUsage(event.code)
      if (code === undefined) return
      if (event.currentTarget === canvas && event.code !== 'Tab' && !event.metaKey) event.preventDefault()
      input({ type: 'key', kind, code, modifiers: modifiers(event) }, at(event))
    }
    const targets: HTMLElement[] = [canvas, textarea]
    for (const target of targets) {
      target.addEventListener('keydown', event => key(event, 0), { signal })
      target.addEventListener('keyup', event => key(event, 1), { signal })
      target.setAttribute(viewKeysAttribute, '')
    }
  }
  canvas.addEventListener('focus', () => {
    if (textArea) textarea.focus({ preventScroll: true })
  }, { signal })

  textarea.addEventListener('compositionstart', () => {
    composing = true
    if (!features.composition) textarea.style.opacity = '1'
  }, { signal })
  textarea.addEventListener('compositionupdate', event => updateComposition(event.data, at(event)), { signal })
  textarea.addEventListener('compositionend', event => {
    composing = false
    textarea.style.opacity = '0'
    if (features.composition) input({ type: 'composition', text: '', cursor: 0 }, at(event))
    composition = ''
    compositionCursor = 0
    if (event.data) input({ type: 'text', text: event.data }, at(event))
    // Some browsers dispatch the final input event after compositionend.
    committed = event.data
    textarea.value = ''
    window.clearTimeout(commitTimer)
    commitTimer = window.setTimeout(() => { committed = undefined }, 0)
  }, { signal })
  textarea.addEventListener('input', event => {
    const change = event as InputEvent
    if (composing || change.isComposing) {
      updateComposition(change.data ?? textarea.value, at(event))
      return
    }
    const text = change.data ?? textarea.value
    textarea.value = ''
    if (text && text !== committed) input({ type: 'text', text }, at(event))
    committed = undefined
  }, { signal })

  if (features.gamepad) {
    const previous = new Map<number, number[]>()
    const changed = (device: Gamepad, timestamp: number) => {
      const pad = device.index
      let buttons = 0
      for (let i = 0; i < 17; ++i) if (device.buttons[i]?.pressed) buttons |= 1 << i
      const [lx = 0, ly = 0, rx = 0, ry = 0] = device.axes
      const lt = device.buttons[6]?.value ?? 0
      const rt = device.buttons[7]?.value ?? 0
      const state = [buttons, lx, ly, rx, ry, lt, rt]
      const prev = previous.get(pad)
      if (!prev || state.some((value, i) => value !== prev[i])) {
        previous.set(pad, state)
        input({ type: 'gamepad', pad, kind: prev ? 1 : 0, buttons, lx, ly, rx, ry, lt, rt }, timestamp)
      }
    }
    const disconnected = (pad: number, timestamp: number) => {
      if (!previous.delete(pad)) return
      input({ type: 'gamepad', pad, kind: 2, buttons: 0, lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0 }, timestamp)
    }
    window.addEventListener('gamepadconnected', event => {
      if (event.gamepad.mapping === 'standard') changed(event.gamepad, at(event))
    }, { signal })
    window.addEventListener('gamepaddisconnected', event => disconnected(event.gamepad.index, at(event)), { signal })
    const gamepads = (timestamp: number) => {
      const seen = new Set<number>()
      for (const device of navigator.getGamepads?.() ?? []) {
        if (!device || device.mapping !== 'standard') continue
        seen.add(device.index)
        changed(device, performance.timeOrigin + (device.timestamp || timestamp))
      }
      for (const pad of previous.keys()) {
        if (!seen.has(pad)) disconnected(pad, performance.timeOrigin + timestamp)
      }
      gamepadFrame = requestAnimationFrame(gamepads)
    }
    gamepadFrame = requestAnimationFrame(gamepads)
  }

  return {
    textArea(area: TextArea | null) {
      textArea = area
      const active = document.activeElement === textarea
      textarea.hidden = !area
      if (!area) {
        if (active) canvas.focus({ preventScroll: true })
        return
      }
      Object.assign(textarea.style, {
        left: `${area.x}px`, top: `${area.y}px`, width: `${Math.max(1, area.width)}px`,
        height: `${Math.max(1, area.height)}px`,
      })
      if (document.activeElement === canvas) textarea.focus({ preventScroll: true })
    },
    dispose() {
      events.abort()
      window.clearTimeout(commitTimer)
      if (gamepadFrame !== undefined) cancelAnimationFrame(gamepadFrame)
      if (document.pointerLockElement === canvas) document.exitPointerLock()
      for (const target of [canvas, textarea]) target.removeAttribute(viewKeysAttribute)
      textarea.hidden = true
    },
  }
}
