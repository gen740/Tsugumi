// Small controls in the shape of their SwiftUI counterparts (Button styles,
// Toggle, segmented Picker, Menu, popover, sheet, ContentUnavailableView, ...).
import { CSSProperties, ReactNode, useEffect, useLayoutEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { Icon } from './icons'
import { L } from '../i18n'

type BtnStyle = 'bordered' | 'prominent' | 'borderless' | 'link' | 'destructive' | 'danger'

export function Button(props: {
  children?: ReactNode
  icon?: string
  onClick?: (e: React.MouseEvent) => void
  disabled?: boolean
  kind?: BtnStyle
  small?: boolean
  large?: boolean
  help?: string
  className?: string
  style?: CSSProperties
  iconOnly?: boolean
  pressed?: boolean
  type?: 'button' | 'submit'
  /** What the guide may point at (guide_show "highlight"). */
  guide?: string
}) {
  const k = props.kind ?? 'bordered'
  return (
    <button
      type={props.type ?? 'button'}
      className={`btn btn-${k}${props.small ? ' btn-small' : ''}${props.large ? ' btn-large' : ''}${props.iconOnly ? ' btn-icon' : ''}${props.pressed ? ' pressed' : ''} ${props.className ?? ''}`}
      disabled={props.disabled}
      title={props.help}
      aria-label={props.iconOnly ? props.help : undefined}
      aria-pressed={props.pressed}
      data-guide={props.guide}
      onClick={(e) => {
        e.stopPropagation()
        props.onClick?.(e)
      }}
      style={props.style}
    >
      {props.icon && <Icon name={props.icon} size={props.small ? 12 : 14} />}
      {props.children !== undefined && props.children !== null && <span>{props.children}</span>}
    </button>
  )
}

export function Toggle({ checked, onChange, label, disabled, kind = 'switch', small, help }: {
  checked: boolean; onChange: (v: boolean) => void; label?: ReactNode; disabled?: boolean; kind?: 'switch' | 'checkbox'; small?: boolean; help?: string
}) {
  return (
    <label className={`toggle toggle-${kind}${small ? ' toggle-small' : ''}${disabled ? ' disabled' : ''}`} title={help} onClick={(e) => e.stopPropagation()}>
      {label !== undefined && kind === 'switch' && <span className="toggle-label">{label}</span>}
      <input type="checkbox" checked={checked} disabled={disabled} onChange={(e) => onChange(e.target.checked)} />
      {kind === 'switch' && <span className="switch" aria-hidden />}
      {label !== undefined && kind === 'checkbox' && <span className="toggle-label">{label}</span>}
    </label>
  )
}

export function Segmented<T extends string | number | null>({ value, options, onChange, width, small }: {
  value: T; options: { value: T; label: ReactNode; help?: string }[]; onChange: (v: T) => void; width?: number; small?: boolean
}) {
  return (
    <div className={`segmented${small ? ' segmented-small' : ''}`} role="radiogroup" style={{ width }}>
      {options.map((o) => (
        <button key={String(o.value)} type="button" role="radio" aria-checked={o.value === value} title={o.help}
          className={o.value === value ? 'on' : ''} onClick={(e) => { e.stopPropagation(); onChange(o.value) }}>
          {o.label}
        </button>
      ))}
    </div>
  )
}

export function Select<T extends string | number>({ value, options, onChange, disabled, label, style, help }: {
  value: T; options: { value: T; label: string; disabled?: boolean }[]; onChange: (v: T) => void; disabled?: boolean; label?: string; style?: CSSProperties; help?: string
}) {
  const sel = (
    <select className="select" value={String(value)} disabled={disabled} title={help} style={style}
      onChange={(e) => {
        const o = options.find((x) => String(x.value) === e.target.value)
        if (o && !o.disabled) onChange(o.value)
      }}>
      {options.map((o) => <option key={String(o.value)} value={String(o.value)} disabled={o.disabled}>{o.label}</option>)}
    </select>
  )
  if (!label) return sel
  return <label className="select-row"><span>{label}</span>{sel}</label>
}

export function TextField({ value, onChange, placeholder, onCommit, disabled, mono, style, autoFocus, small, help, multiline, rows, onKeyDown, inputRef }: {
  value: string; onChange: (v: string) => void; placeholder?: string; onCommit?: (v: string) => void; disabled?: boolean; mono?: boolean
  style?: CSSProperties; autoFocus?: boolean; small?: boolean; help?: string; multiline?: boolean; rows?: number
  onKeyDown?: (e: React.KeyboardEvent) => void; inputRef?: React.Ref<HTMLInputElement & HTMLTextAreaElement>
}) {
  const cls = `field${mono ? ' mono' : ''}${small ? ' field-small' : ''}`
  if (multiline) {
    return <textarea ref={inputRef} className={cls} value={value} rows={rows ?? 3} placeholder={placeholder} disabled={disabled} title={help} style={style}
      autoFocus={autoFocus} onChange={(e) => onChange(e.target.value)} onBlur={() => onCommit?.(value)} onKeyDown={onKeyDown} />
  }
  return (
    <input ref={inputRef} className={cls} value={value} placeholder={placeholder} disabled={disabled} title={help} style={style} autoFocus={autoFocus}
      spellCheck={false} onChange={(e) => onChange(e.target.value)} onBlur={() => onCommit?.(value)}
      onKeyDown={(e) => {
        if (e.key === 'Enter' && !e.nativeEvent.isComposing) onCommit?.((e.target as HTMLInputElement).value)
        onKeyDown?.(e)
      }} />
  )
}

export function Stepper({ value, onChange, min, max, step = 1, label }: { value: number; onChange: (v: number) => void; min: number; max: number; step?: number; label: ReactNode }) {
  return (
    <div className="stepper">
      <span>{label}</span>
      <div className="stepper-buttons">
        <button type="button" disabled={value <= min} onClick={() => onChange(Math.max(min, value - step))} aria-label={L('減らす')}>−</button>
        <button type="button" disabled={value >= max} onClick={() => onChange(Math.min(max, value + step))} aria-label={L('増やす')}>+</button>
      </div>
    </div>
  )
}

export function Spinner({ small }: { small?: boolean }) {
  return <span className={`spinner${small ? ' spinner-small' : ''}`} role="progressbar" aria-label={L('読み込み中')} />
}

/** ContentUnavailableView */
export function Empty({ title, icon, description, children, small }: { title: string; icon?: string; description?: ReactNode; children?: ReactNode; small?: boolean }) {
  return (
    <div className={`empty${small ? ' empty-small' : ''}`}>
      {icon && <Icon name={icon} size={small ? 22 : 34} />}
      <div className="empty-title">{title}</div>
      {description && <div className="empty-desc">{description}</div>}
      {children}
    </div>
  )
}

export function Divider({ vertical }: { vertical?: boolean }) {
  return <div className={vertical ? 'vdivider' : 'divider'} />
}

/** Grouped form (Form.formStyle(.grouped)). */
export function Form({ children, style }: { children: ReactNode; style?: CSSProperties }) {
  return <div className="form" style={style}>{children}</div>
}
export function Section({ title, children, footer }: { title?: ReactNode; children: ReactNode; footer?: ReactNode }) {
  return (
    <section className="form-section">
      {title && <div className="form-section-title">{title}</div>}
      <div className="form-box">{children}</div>
      {footer && <div className="form-footer">{footer}</div>}
    </section>
  )
}
export function Row({ label, children, help }: { label?: ReactNode; children?: ReactNode; help?: string }) {
  return (
    <div className="form-row" title={help}>
      {label !== undefined && <span className="form-label">{label}</span>}
      <div className="form-value">{children}</div>
    </div>
  )
}
/** LabeledContent */
export function Value({ label, value }: { label: ReactNode; value: ReactNode }) {
  return <Row label={label}><span className="secondary selectable">{value}</span></Row>
}

export function Disclosure({ label, children, initiallyOpen = false }: { label: ReactNode; children: ReactNode; initiallyOpen?: boolean }) {
  const [open, setOpen] = useState(initiallyOpen)
  return (
    <div className="disclosure">
      <button type="button" className="disclosure-head" onClick={() => setOpen(!open)} aria-expanded={open}>
        <Icon name={open ? 'chevron.down' : 'chevron.right'} size={10} />
        {label}
      </button>
      {open && <div className="disclosure-body">{children}</div>}
    </div>
  )
}

// ---- Menus ----------------------------------------------------------------------

export type MenuItem =
  | { label: string; action?: () => void; disabled?: boolean; destructive?: boolean; checked?: boolean; shortcut?: string; submenu?: MenuItem[] }
  | 'divider'
  | null
  | false
  | undefined

let showMenuImpl: ((x: number, y: number, items: MenuItem[]) => void) | undefined
let updateMenuImpl: ((serial: number, items: MenuItem[]) => void) | undefined
let menuSerial = 0

/** Opens a context menu at the pointer. `update` changes its items while it
 *  is open, and does nothing once it has closed. */
export function showMenu(e: { clientX: number; clientY: number; preventDefault?: () => void; stopPropagation?: () => void }, items: MenuItem[]) {
  e.preventDefault?.()
  e.stopPropagation?.()
  const serial = ++menuSerial
  showMenuImpl?.(e.clientX, e.clientY, items)
  return { update: (next: MenuItem[]) => updateMenuImpl?.(serial, next) }
}

function MenuList({ items, x, y, onDone }: { items: MenuItem[]; x: number; y: number; onDone: () => void }) {
  const ref = useRef<HTMLDivElement>(null)
  const [pos, setPos] = useState({ x, y })
  const [sub, setSub] = useState<{ i: number; x: number; y: number } | null>(null)
  useLayoutEffect(() => {
    const r = ref.current?.getBoundingClientRect()
    if (!r) return
    setPos({ x: Math.max(4, Math.min(x, window.innerWidth - r.width - 6)), y: Math.max(4, Math.min(y, window.innerHeight - r.height - 6)) })
  }, [x, y, items.length])
  const list = items.filter((i): i is Exclude<MenuItem, null | false | undefined> => !!i)
  return (
    <div className="menu" ref={ref} style={{ left: pos.x, top: pos.y }} role="menu" onContextMenu={(e) => e.preventDefault()}>
      {list.map((it, i) =>
        it === 'divider' ? <div key={i} className="menu-divider" /> : (
          <button key={i} type="button" role="menuitem" className={`menu-item${it.destructive ? ' destructive' : ''}`} disabled={it.disabled}
            onMouseEnter={(e) => {
              if (it.submenu) {
                const r = (e.currentTarget as HTMLElement).getBoundingClientRect()
                setSub({ i, x: r.right - 2, y: r.top - 4 })
              } else setSub(null)
            }}
            onClick={() => {
              if (it.submenu) return
              onDone()
              it.action?.()
            }}>
            <span className="menu-check">{it.checked ? '✓' : ''}</span>
            <span className="menu-label">{it.label}</span>
            {it.shortcut && <span className="menu-shortcut">{it.shortcut}</span>}
            {it.submenu && <Icon name="chevron.right" size={10} />}
          </button>
        ),
      )}
      {sub && list[sub.i] !== 'divider' && (list[sub.i] as any).submenu && (
        <MenuList items={(list[sub.i] as any).submenu} x={sub.x} y={sub.y} onDone={onDone} />
      )}
    </div>
  )
}

export function MenuHost() {
  const [m, setM] = useState<{ serial: number; x: number; y: number; items: MenuItem[] } | null>(null)
  useEffect(() => {
    showMenuImpl = (x, y, items) => setM({ serial: menuSerial, x, y, items })
    updateMenuImpl = (serial, items) => setM((cur) => (cur?.serial === serial ? { ...cur, items } : cur))
    return () => {
      showMenuImpl = undefined
      updateMenuImpl = undefined
    }
  }, [])
  useEffect(() => {
    if (!m) return
    const close = () => setM(null)
    const key = (e: KeyboardEvent) => e.key === 'Escape' && close()
    window.addEventListener('keydown', key, true)
    window.addEventListener('blur', close)
    return () => {
      window.removeEventListener('keydown', key, true)
      window.removeEventListener('blur', close)
    }
  }, [m])
  if (!m) return null
  return createPortal(
    <div className="menu-backdrop" onMouseDown={(e) => { if (e.target === e.currentTarget) setM(null) }} onContextMenu={(e) => { e.preventDefault(); setM(null) }}>
      <MenuList items={m.items} x={m.x} y={m.y} onDone={() => setM(null)} />
    </div>,
    document.body,
  )
}

/** A button that opens a menu below itself (Menu with a label). */
export function MenuButton({ icon, label, items, disabled, help, kind = 'borderless' }: { icon?: string; label?: ReactNode; items: () => MenuItem[]; disabled?: boolean; help?: string; kind?: BtnStyle }) {
  return (
    <Button kind={kind} icon={icon} disabled={disabled} help={help} iconOnly={!label}
      onClick={(e) => {
        const r = (e.currentTarget as HTMLElement).getBoundingClientRect()
        showMenu({ clientX: r.left, clientY: r.bottom + 2 }, items())
      }}>
      {label}
    </Button>
  )
}

// ---- Popover and sheets -------------------------------------------------------------

/** A popover anchored to its trigger (opens upward when there is no room below). */
export function Popover({ open, onClose, anchor, children, width, height }: {
  open: boolean; onClose: () => void; anchor: React.RefObject<HTMLElement | null>; children: ReactNode; width?: number; height?: number
}) {
  const ref = useRef<HTMLDivElement>(null)
  const [pos, setPos] = useState<{ left: number; top: number } | null>(null)
  useLayoutEffect(() => {
    if (!open) return
    const a = anchor.current?.getBoundingClientRect()
    const r = ref.current?.getBoundingClientRect()
    if (!a || !r) return
    let top = a.bottom + 8
    if (top + r.height > window.innerHeight - 8) top = Math.max(8, a.top - r.height - 8)
    const left = Math.max(8, Math.min(a.left + a.width / 2 - r.width / 2, window.innerWidth - r.width - 8))
    setPos({ left, top })
  }, [open, anchor])
  useEffect(() => {
    if (!open) return
    const down = (e: MouseEvent) => {
      if (!ref.current?.contains(e.target as Node) && !anchor.current?.contains(e.target as Node)) onClose()
    }
    const key = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation()
        onClose()
      }
    }
    window.addEventListener('mousedown', down, true)
    window.addEventListener('keydown', key, true)
    return () => {
      window.removeEventListener('mousedown', down, true)
      window.removeEventListener('keydown', key, true)
    }
  }, [open, onClose, anchor])
  if (!open) return null
  return createPortal(
    <div ref={ref} className="popover" style={{ left: pos?.left ?? -9999, top: pos?.top ?? -9999, width, height }}>
      {children}
    </div>,
    document.body,
  )
}

/** A modal sheet over its window (or the page). */
export function Sheet({ children, onClose, width, backdropCloses = true }: { children: ReactNode; onClose: () => void; width?: number; backdropCloses?: boolean }) {
  useEffect(() => {
    const key = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation()
        onClose()
      }
    }
    window.addEventListener('keydown', key, true)
    return () => window.removeEventListener('keydown', key, true)
  }, [onClose])
  return createPortal(
    <div className="sheet-backdrop" onMouseDown={(e) => backdropCloses && e.target === e.currentTarget && onClose()}>
      <div className="sheet" style={{ width }} role="dialog" aria-modal>{children}</div>
    </div>,
    document.body,
  )
}

/** confirmationDialog: a destructive choice with an explanation. */
export function Confirm({ title, message, confirm, onConfirm, onCancel }: { title: string; message?: string; confirm: string; onConfirm: () => void; onCancel: () => void }) {
  return (
    <Sheet onClose={onCancel} width={420}>
      <div className="confirm">
        <div className="confirm-title">{title}</div>
        {message && <div className="secondary">{message}</div>}
        <div className="row end gap8">
          <Button onClick={onCancel}>{L('キャンセル')}</Button>
          <Button kind="danger" onClick={() => { onCancel(); onConfirm() }}>{confirm}</Button>
        </div>
      </div>
    </Sheet>
  )
}

/** alert with a text field (create / rename). */
export function Prompt({ title, initial, placeholder, confirm, onConfirm, onCancel }: { title: string; initial: string; placeholder?: string; confirm: string; onConfirm: (v: string) => void; onCancel: () => void }) {
  const [v, setV] = useState(initial)
  return (
    <Sheet onClose={onCancel} width={380}>
      <form className="confirm" onSubmit={(e) => { e.preventDefault(); onCancel(); onConfirm(v) }}>
        <div className="confirm-title">{title}</div>
        <TextField value={v} onChange={setV} placeholder={placeholder} autoFocus />
        <div className="row end gap8">
          <Button onClick={onCancel}>{L('キャンセル')}</Button>
          <Button kind="prominent" type="submit" disabled={!v}>{confirm}</Button>
        </div>
      </form>
    </Sheet>
  )
}

/** Horizontal split with draggable dividers (HSplitView). */
export function HSplit({ children, sizes, min, onResize, storageKey }: {
  children: ReactNode[]; sizes: (number | null)[]; min?: number[]; onResize?: (s: (number | null)[]) => void; storageKey?: string
}) {
  const [w, setW] = useState<(number | null)[]>(() => {
    if (storageKey) {
      try {
        const v = JSON.parse(localStorage.getItem(storageKey) ?? 'null')
        if (Array.isArray(v) && v.length === sizes.length) return v
      } catch {}
    }
    return sizes
  })
  const kids = children.filter(Boolean)
  return (
    <div className="hsplit">
      {kids.map((c, i) => (
        <SplitPane key={i} width={w[i]} flex={w[i] === null}>
          {c}
          {i < kids.length - 1 && (
            <div className="hsplit-handle" onMouseDown={(e) => {
              e.preventDefault()
              const startX = e.clientX
              // The fixed pane next to the handle is resized; a flexible one takes the rest.
              const target = w[i] !== null ? i : i + 1
              const sign = target === i ? 1 : -1
              const pane = (e.currentTarget.parentElement?.parentElement?.children[target] as HTMLElement)
              const start = pane?.getBoundingClientRect().width ?? 200
              const move = (m: MouseEvent) => {
                const nw = Math.max(min?.[target] ?? 120, start + sign * (m.clientX - startX))
                setW((old) => {
                  const n = [...old]
                  n[target] = nw
                  onResize?.(n)
                  if (storageKey) try { localStorage.setItem(storageKey, JSON.stringify(n)) } catch {}
                  return n
                })
              }
              const up = () => {
                window.removeEventListener('mousemove', move)
                window.removeEventListener('mouseup', up)
                document.body.classList.remove('resizing-h')
              }
              document.body.classList.add('resizing-h')
              window.addEventListener('mousemove', move)
              window.addEventListener('mouseup', up)
            }} />
          )}
        </SplitPane>
      ))}
    </div>
  )
}

function SplitPane({ children, width, flex }: { children: ReactNode; width: number | null; flex: boolean }) {
  return <div className="hsplit-pane" style={flex ? { flex: 1, minWidth: 0 } : { width: width ?? undefined, flex: 'none' }}>{children}</div>
}

export async function copyText(t: string) {
  try {
    await navigator.clipboard.writeText(t)
  } catch {
    const ta = document.createElement('textarea')
    ta.value = t
    document.body.appendChild(ta)
    ta.select()
    document.execCommand('copy')
    ta.remove()
  }
}

/** Saves bytes as a download (the core never receives a local path). */
export function download(name: string, data: Uint8Array | string, type = 'application/octet-stream') {
  const blob = new Blob([data as BlobPart], { type })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = name
  a.click()
  setTimeout(() => URL.revokeObjectURL(url), 5000)
}
