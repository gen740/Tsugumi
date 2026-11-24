// USB HID Keyboard/Keypad usages for standard KeyboardEvent.code values.
const usages = new Map<string, number>([
  ['Enter', 0x28], ['Escape', 0x29], ['Backspace', 0x2a], ['Tab', 0x2b], ['Space', 0x2c],
  ['Minus', 0x2d], ['Equal', 0x2e], ['BracketLeft', 0x2f], ['BracketRight', 0x30], ['Backslash', 0x31],
  ['Semicolon', 0x33], ['Quote', 0x34], ['Backquote', 0x35], ['Comma', 0x36], ['Period', 0x37],
  ['Slash', 0x38], ['CapsLock', 0x39], ['PrintScreen', 0x46], ['ScrollLock', 0x47], ['Pause', 0x48],
  ['Insert', 0x49], ['Home', 0x4a], ['PageUp', 0x4b], ['Delete', 0x4c], ['End', 0x4d], ['PageDown', 0x4e],
  ['ArrowRight', 0x4f], ['ArrowLeft', 0x50], ['ArrowDown', 0x51], ['ArrowUp', 0x52],
  ['NumLock', 0x53], ['NumpadDivide', 0x54], ['NumpadMultiply', 0x55], ['NumpadSubtract', 0x56],
  ['NumpadAdd', 0x57], ['NumpadEnter', 0x58], ['NumpadDecimal', 0x63],
  ['IntlBackslash', 0x64], ['ContextMenu', 0x65], ['NumpadEqual', 0x67], ['NumpadComma', 0x85],
  ['IntlRo', 0x87], ['KanaMode', 0x88], ['IntlYen', 0x89], ['Convert', 0x8a], ['NonConvert', 0x8b],
  ['ControlLeft', 0xe0], ['ShiftLeft', 0xe1], ['AltLeft', 0xe2], ['MetaLeft', 0xe3],
  ['ControlRight', 0xe4], ['ShiftRight', 0xe5], ['AltRight', 0xe6], ['MetaRight', 0xe7],
])
for (const [index, letter] of [...'ABCDEFGHIJKLMNOPQRSTUVWXYZ'].entries()) usages.set(`Key${letter}`, 0x04 + index)
for (const [index, digit] of [...'1234567890'].entries()) {
  usages.set(`Digit${digit}`, 0x1e + index)
  usages.set(`Numpad${digit}`, 0x59 + index)
}
for (let key = 1; key <= 12; ++key) usages.set(`F${key}`, 0x39 + key)

/** Marks a view canvas that takes keys: while it has the focus, every key is the view's. */
export const viewKeysAttribute = 'data-view-keys'

export function viewOwnsKeys(focused: Pick<Element, 'closest'> | null): boolean {
  return !!focused?.closest(`[${viewKeysAttribute}]`)
}

export const keyUsage = (code: string): number | undefined => usages.get(code)

export function modifiers(event: MouseEvent | KeyboardEvent): number {
  return +event.shiftKey | (+event.ctrlKey << 1) | (+event.altKey << 2) | (+event.metaKey << 3)
}

export function wheelPixels(event: WheelEvent, canvas: HTMLCanvasElement): [number, number] {
  if (event.deltaMode === WheelEvent.DOM_DELTA_PAGE)
    return [event.deltaX * canvas.clientWidth, event.deltaY * canvas.clientHeight]
  const scale = event.deltaMode === WheelEvent.DOM_DELTA_LINE
    ? parseFloat(getComputedStyle(canvas).lineHeight) || 16
    : 1
  return [event.deltaX * scale, event.deltaY * scale]
}
