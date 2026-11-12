import { activeNixLinks, nixCurve, nixLinks as linksIn, nixReferences } from '../model/nixReferences'
// The canvas of one scope: grid, groups, connections, Cards, sticky notes
// and output frames, with pan/zoom, selection, wiring and drag & drop.
// Inside a Card in Card, the outer Card's inputs and outputs are drawn as
// bars above and below the inner graph.
import { CSSProperties, ReactNode, useEffect, useLayoutEffect, useRef, useState } from 'react'
import { app } from '../model/app'
import { HelperBirds } from './helperBirds'
import { WritingPanels } from './writingPanel'
import { Boundary, canvasFor, linkEnds, midPoint, noteRect, pathToScreen, SceneDrag } from '../model/canvas'
import { NEW_PORT, Pt, Rect, snapToGrid } from '../model/canvasKit'
import { useObserve } from '../model/observable'
import { holdsDisplay } from '../model/moduleThumbnails'
import { CANVAS_ONLY, prefs, ShortcutAction } from '../model/prefs'
import {
  CardKey, DefinitionTemplate, key, KIND_ICON, KIND_LABEL, NOTE_COLORS, parseParam, path, ProjectSession, TEMPLATES, templateIcon,
} from '../model/session'
import { Workspace, WorkflowTab } from '../model/workspace'
import { arr, dbl, get, J, JObj, obj, real, str } from '../shared/json'
import {
  DEFINITION_DRAG, EXTERNAL_DEFINITION_DRAG, EXTERNAL_DRAG, openCard, openWorkflowWindow, runCard, showLog, TEMPLATE_DRAG,
} from './actions'
import { BoundaryBarView, CardNodeView, drive, gridStyle, idleText, runText, WireLayer } from './cardNode'
import { retargetWindows } from './cardSettings'
import { Button, MenuItem, Segmented, Sheet, showMenu, Stepper, TextField, Toggle } from './controls'
import { DisplayContent } from './display'
import { Icon } from './icons'
import { Markdown } from './markdown'
import { L } from '../i18n'
import { visualizationTemplates } from '../model/visualizationTemplates'

// Kept here for the views that import them from the canvas.
export { DEFINITION_DRAG, DisplayContent, openWorkflowWindow }
export const kindIcon = (k: string) => (KIND_ICON as Record<string, string>)[k] ?? 'circle'
export const noteColor = (id: J | string | undefined) => {
  const c = typeof id === 'string' ? id : str(id as J | undefined)
  return `var(--note-${c && NOTE_COLORS.some((n) => n.id === c) ? c : 'yellow'})`
}
const tint = (color: string, opacity: number) => `color-mix(in srgb, ${color} ${Math.round(opacity * 100)}%, transparent)`

type HoverTarget = { kind: 'card'; k: CardKey } | { kind: 'wire'; i: number }
const sameTarget = (a?: HoverTarget, b?: HoverTarget) =>
  a === b || (!!a && !!b && a.kind === b.kind && (a.kind === 'card' ? a.k === (b as { k: CardKey }).k : a.i === (b as { i: number }).i))

/** Places a view drawn at screen scale with its top-left corner on whole device pixels. */
function placed(p: Pt, w: number, h: number, extra?: CSSProperties): CSSProperties {
  const px = window.devicePixelRatio || 1
  return { position: 'absolute', left: Math.round(p.x * px) / px, top: Math.round(p.y * px) / px, width: w, height: h, ...extra }
}

const isTyping = (t: EventTarget | null) => !!(t as HTMLElement | null)?.closest?.('input, textarea, select, [contenteditable="true"]')

// ---- The canvas ---------------------------------------------------------------------

export function GraphCanvas({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  const cv = canvasFor(session, tab)
  useObserve(session, tab, cv, prefs)
  const root = useRef<HTMLDivElement>(null)
  const [cardDrag, setCardDrag] = useState<SceneDrag['cards']>()
  const [boundaryDrag, setBoundaryDrag] = useState<SceneDrag['boundary']>()
  /** A wire being drawn from a port: from an output (to an input), or from an input back to an output. */
  const [pending, setPending] = useState<{ from: [CardKey, string]; fromOutput: boolean; point: Pt }>()
  const [marquee, setMarquee] = useState<{ start: Pt; end: Pt }>()
  const [panning, setPanning] = useState(false)
  const [frameDrag, setFrameDrag] = useState<{ card: CardKey; dx: number; dy: number }>()
  const [noteDrag, setNoteDrag] = useState<{ id: string; dx: number; dy: number }>()
  const [hover, setHover] = useState<HoverTarget>()
  const [hoverAt, setHoverAt] = useState<Pt>({ x: 0, y: 0 })
  const [bubbleSize, setBubbleSize] = useState({ width: 300, height: 120 })
  const [spaceHeld, setSpaceHeld] = useState(false)
  const wanted = useRef<HoverTarget>()
  const hoverTimer = useRef<ReturnType<typeof setTimeout>>()
  const pointer = useRef<Pt>({ x: 0, y: 0 })
  const bubble = useRef<HTMLDivElement>(null)
  const findField = useRef<HTMLInputElement & HTMLTextAreaElement>(null)

  const scene = cv.scene({ cards: cardDrag, boundary: boundaryDrag })
  const nixLinks = tab.showNixReferences ? linksIn(nixReferences(session), tab.scope, new Set(scene.cardKeys)) : []
  const activeNix = activeNixLinks(nixLinks, tab.hoveredNixCard, tab.selectedNixReference)
  const highlightedNix = new Set(activeNix.flatMap(ref => [ref.source, ref.target]))
  const cam = cv.camera
  const z = cam.zoom
  const scope = tab.scope
  const interacting = !!(cardDrag || pending || marquee || panning || boundaryDrag || frameDrag || noteDrag)
  const screen = (w: Pt) => cv.screen(w)
  const local = (e: { clientX: number; clientY: number }): Pt => {
    const r = root.current?.getBoundingClientRect()
    return { x: e.clientX - (r?.left ?? 0), y: e.clientY - (r?.top ?? 0) }
  }
  const focus = () => root.current?.focus({ preventScroll: true })

  // Size, and the whole flow for a scope shown for the first time (no saved camera).
  useLayoutEffect(() => {
    const el = root.current
    if (!el) return
    const update = () => {
      cv.size = { width: el.clientWidth, height: el.clientHeight }
      cv.fitIfNew()
      cv.changed()
    }
    const o = new ResizeObserver(update)
    o.observe(el)
    update()
    return () => o.disconnect()
  }, [cv])
  useEffect(() => cv.fitIfNew(), [cv, scene.cardKeys.length, scope.join('/')])

  // Wheel: zoom around the pointer (mouse wheel, pinch, ⌘-scroll); trackpad scroll pans.
  useEffect(() => {
    const el = root.current
    if (!el) return
    const onWheel = (e: WheelEvent) => {
      if ((e.target as HTMLElement).closest('.cv-find, .display-scroll, .cv-draft-edit')) return
      e.preventDefault()
      clearHover()
      const r = el.getBoundingClientRect()
      const p = { x: e.clientX - r.left, y: e.clientY - r.top }
      const wd = (e as WheelEvent & { wheelDeltaY?: number }).wheelDeltaY
      // A mouse wheel steps by lines: Firefox says so (deltaMode), Chrome gives 120 per
      // line. Chrome gives a trackpad's pixels as 3 × deltaY, which is a multiple of 120
      // when the trackpad moves by 40 px: that one is not a wheel.
      const mouse = e.deltaMode !== 0 || (typeof wd === 'number' && wd !== 0 && Math.abs(wd) % 120 === 0 && wd !== -3 * e.deltaY && e.deltaX === 0)
      // As on the Mac, a wheel zooms by the same steps with ⌘ or without.
      if (mouse) cv.zoomBy(Math.max(0.5, 1 + (e.deltaMode === 1 ? -e.deltaY : (wd ?? -e.deltaY) / 120) * 0.08), p)
      else if (e.ctrlKey || e.metaKey) cv.zoomBy(Math.exp(-e.deltaY * 0.01), p)
      else {
        const c = cv.camera
        cv.setCamera({ ...c, x: c.x - e.deltaX, y: c.y - e.deltaY })
      }
    }
    el.addEventListener('wheel', onWheel, { passive: false })
    return () => el.removeEventListener('wheel', onWheel)
  }, [cv])

  useEffect(() => {
    if (cv.findText !== undefined) findField.current?.focus()
  }, [cv.findRequest, cv.findText === undefined])

  useLayoutEffect(() => {
    const r = bubble.current?.getBoundingClientRect()
    if (r && (Math.abs(r.width - bubbleSize.width) > 0.5 || Math.abs(r.height - bubbleSize.height) > 0.5)) setBubbleSize({ width: r.width, height: r.height })
  })

  // ---- Hover details ----

  const clearHover = () => {
    clearTimeout(hoverTimer.current)
    wanted.current = undefined
    setHover(undefined)
    tab.hoverNixCard(undefined)
  }

  const track = (p: Pt) => {
    pointer.current = p
    if (interacting) return clearHover()
    const w = cv.world(p)
    let target: HoverTarget | undefined
    const k = scene.cardAt(w)
    tab.hoverNixCard(k)
    // A Draft being written has its details in front of it.
    if (k) target = k === session.inlineDraft?.card ? undefined : { kind: 'card', k }
    else {
      // About 8 points on screen either side of the line, at any zoom.
      const i = scene.wireAt(w, 8 / z)
      if (i !== undefined) target = { kind: 'wire', i }
    }
    if (cv.details !== undefined && !(target?.kind === 'card' && target.k === cv.details)) cv.set('details', undefined)
    if (sameTarget(target, wanted.current)) {
      if (hover) setHoverAt(p)
      return
    }
    clearTimeout(hoverTimer.current)
    wanted.current = target
    if (hover) setHover(undefined)
    if (!target) return
    // A short wait, so moving across the canvas does not flash details.
    hoverTimer.current = setTimeout(() => {
      setHoverAt(pointer.current)
      setHover(target)
    }, 450)
  }

  // ---- Actions ----

  const select = (k: CardKey, toggle: boolean) => {
    if (toggle) {
      const n = new Set(tab.selection)
      if (n.has(k)) n.delete(k)
      else n.add(k)
      tab.selectedConnection = undefined
      // Adding to the selection keeps what a marquee caught.
      tab.setSelection(n, tab.selectedNotes, tab.selectedFrames)
    } else {
      tab.selectedConnection = undefined
      tab.setSelection([k])
    }
  }

  const backgroundTap = (p: Pt) => {
    // A click near a wire selects it (the path as drawn); elsewhere clears the selection.
    const i = scene.wireAt(cv.world(p), Math.max(5, 7 / z))
    if (i !== undefined) {
      tab.selectedConnection = i
      tab.setSelection([])
      return
    }
    tab.selectedConnection = undefined
    tab.setSelection([])
  }

  /** The port under a point (screen), within a finger's width. */
  const nearestPort = (p: Pt, output: boolean, except: CardKey): [CardKey, string] | undefined => {
    const w = cv.world(p)
    let best: [CardKey, string] | undefined, bestD = Infinity
    for (const k of scene.cardKeys) {
      if (k === except) continue
      const g = scene.geometry(k)
      for (const port of output ? g.outputs : g.inputs) {
        const pt = scene.portWorld(k, port, output)
        const d = Math.hypot(pt.x - w.x, pt.y - w.y)
        if (d < 18 / z + 6 && d < bestD) {
          best = [k, port]
          bestD = d
        }
      }
    }
    return best
  }

  /** Where a dropped wire ends. A Draft takes it anywhere on the Card: a port
   *  of its own that has no wire, or a new one (never replacing another wire). */
  const wireTarget = (p: Pt, output: boolean, except: CardKey): [CardKey, string] | undefined => {
    const near = nearestPort(p, output, except)
    if (near) return session.kind(near[0]) === 'draft' && session.portWired(near[0], near[1], output, scope) ? [near[0], NEW_PORT] : near
    const w = cv.world(p), m = 18 / z + 6
    const k = [...scene.cardKeys].reverse().find((x) => {
      if (x === except || session.kind(x) !== 'draft') return false
      const r = scene.cardRect(x)
      return w.x >= r.x - m && w.x <= r.x + r.width + m && w.y >= r.y - m && w.y <= r.y + r.height + m
    })
    return k ? [k, NEW_PORT] : undefined
  }

  /** Keeps what was written on a Draft's Card; a new name moves what points at the Card. */
  const finishDraftEdit = () => {
    const from = session.inlineDraft?.card
    void session.finishDraftEdit().then((to) => {
      if (!from || !to) return
      retargetWindows(session, from, to)
      if (tab.selection.has(from)) tab.setSelection([...tab.selection].map((x) => (x === from ? to : x)))
    })
  }

  const commitParam = (k: CardKey, name: string, text: string) => {
    if (!session.isEditable) return
    const type = session.kind(k) === 'switch' && name === 'value' ? 'bool'
      : str(get((arr(get(session.cardDefinition(k), 'params')) ?? []).find((p) => str(get(p, 'name')) === name), 'type'))
    if (type) session.setParam(k, name, parseParam(text, type))
  }

  const addBuiltin = (t: DefinitionTemplate, at?: Pt) => {
    const k = cv.addBuiltin(t, at)
    if (k && t === 'draft') cv.writeDraft(k)
  }

  const single = () => (tab.selection.size === 1 ? [...tab.selection][0] : undefined)

  const canvasAction = (a: ShortcutAction) => {
    const k = single()
    switch (a) {
      case 'openSettings': if (k) app.openWindow(session, { kind: 'card', card: k }); break
      case 'openArtifacts': if (k) app.openWindow(session, { kind: 'artifacts', card: k }); break
      case 'openLog': if (k) showLog(workspace, k); break
      case 'addDraft': {
        const d = cv.addDraft()
        if (d) cv.writeDraft(d)
        break
      }
      case 'generateDrafts': {
        if (session.dirty || session.runActive) break
        const drafts = scene.cardKeys.filter((x) => session.kind(x) === 'draft')
        const chosen = drafts.filter((x) => tab.selection.has(x))
        workspace.set('showChat', true)
        app.chat(session).generateDrafts(scope, chosen.length ? chosen : drafts)
        break
      }
      default: cv.handle(a)
    }
  }

  /** Esc: cancel what is going on, then clear the selection. */
  const escape = () => {
    if (pending) setPending(undefined)
    else if (cv.findText !== undefined) cv.set('findText', undefined)
    else if (hover || cv.details !== undefined) {
      clearHover()
      cv.set('details', undefined)
    } else cv.escape()
  }

  const onKeyDown = (e: React.KeyboardEvent) => {
    // Keys typed in a field, or in an editor sheet (a portal of the canvas), are theirs.
    if (isTyping(e.target) || !root.current?.contains(e.target as Node)) return
    if (document.querySelector('.menu-backdrop')) return
    if (e.key === ' ') {
      e.preventDefault()
      if (!spaceHeld) setSpaceHeld(true)
      return
    }
    if (e.key === 'Escape') {
      e.preventDefault()
      return escape()
    }
    if ((e.key === 'Backspace' || e.key === 'Delete') && !e.metaKey && !e.ctrlKey && !e.altKey) {
      e.preventDefault()
      return cv.deleteSelection()
    }
    const a = prefs.action(e.nativeEvent, true)
    if (!a || !(CANVAS_ONLY.has(a) || a === 'find' || a === 'showDetails' || a === 'addNote')) return
    e.preventDefault()
    e.stopPropagation()
    canvasAction(a)
  }

  // ---- Pointer gestures ----

  const startPan = (e: React.PointerEvent) => {
    const start = { x: e.clientX, y: e.clientY, cx: cam.x, cy: cam.y }
    clearHover()
    drive(e, 2, (ev) => {
      setPanning(true)
      cv.setCamera({ ...cv.camera, x: start.cx + ev.clientX - start.x, y: start.cy + ev.clientY - start.y }, false)
    }, (_ev, moved) => {
      setPanning(false)
      if (moved) cv.saveView()
    })
  }

  // Middle button, or Space + drag: pan anywhere on the canvas.
  const onPointerDownCapture = (e: React.PointerEvent) => {
    if (e.button === 1 || (e.button === 0 && spaceHeld)) {
      e.preventDefault()
      e.stopPropagation()
      focus()
      startPan(e)
    }
  }

  const onBackgroundDown = (e: React.PointerEvent) => {
    if (e.button !== 0) return
    focus()
    clearHover()
    const start = local(e)
    if (e.shiftKey) {
      drive(e, 2, (ev) => setMarquee({ start, end: local(ev) }), (ev, moved) => {
        setMarquee(undefined)
        if (!moved) return backgroundTap(start)
        const a = cv.world(start), b = cv.world(local(ev))
        const r = { x: Math.min(a.x, b.x), y: Math.min(a.y, b.y), width: Math.abs(b.x - a.x), height: Math.abs(b.y - a.y) }
        tab.selectedConnection = undefined
        const items = scene.itemsIn(r)
        tab.setSelection(items.cards, items.notes, items.frames)
      })
      return
    }
    const cam0 = { ...cam }
    drive(e, 2, (ev) => {
      setPanning(true)
      cv.setCamera({ ...cv.camera, x: cam0.x + ev.clientX - e.clientX, y: cam0.y + ev.clientY - e.clientY }, false)
    }, (_ev, moved) => {
      setPanning(false)
      if (moved) cv.saveView()
      else backgroundTap(start)
    })
  }

  const onCardDown = (k: CardKey, e: React.PointerEvent) => {
    if (e.button !== 0) return
    e.stopPropagation()
    focus()
    const start = { x: e.clientX, y: e.clientY }
    const toggle = e.shiftKey || e.metaKey
    let keys: Set<CardKey> | undefined
    drive(e, 3, (ev) => {
      clearHover()
      if (!keys) {
        if (!tab.selection.has(k)) tab.setSelection([k])
        keys = new Set(tab.selection)
      }
      setCardDrag({ keys, frames: tab.selectedFrames, dx: (ev.clientX - start.x) / z, dy: (ev.clientY - start.y) / z })
    }, (ev, moved) => {
      setCardDrag(undefined)
      if (!moved || !keys) return select(k, toggle)
      moveSelection((ev.clientX - start.x) / z, (ev.clientY - start.y) / z)
    })
  }

  const moveSelection = (dx: number, dy: number) =>
    cv.moveSelection(tab.selection, tab.selectedNotes, tab.selectedFrames, dx, dy, prefs.showGrid ? prefs.gridSize : undefined)

  /** Drags the selection from one of its notes or frames. */
  const dragSelection = (e: React.PointerEvent) => {
    const start = { x: e.clientX, y: e.clientY }
    drive(e, 3, (ev) => {
      clearHover()
      setCardDrag({ keys: tab.selection, frames: tab.selectedFrames, dx: (ev.clientX - start.x) / z, dy: (ev.clientY - start.y) / z })
    }, (ev, moved) => {
      setCardDrag(undefined)
      if (moved) moveSelection((ev.clientX - start.x) / z, (ev.clientY - start.y) / z)
    })
  }

  const onPortDown = (k: CardKey, port: string, output: boolean, e: React.PointerEvent) => {
    focus()
    if (!session.isEditable) return
    clearHover()
    setPending({ from: [k, port], fromOutput: output, point: local(e) })
    drive(e, 1, (ev) => setPending({ from: [k, port], fromOutput: output, point: local(ev) }), (ev) => {
      setPending(undefined)
      if (!session.isEditable) return
      const other = wireTarget(local(ev), !output, k)
      if (!other) return
      // Another wire from a Draft's port is another port.
      const own: [CardKey, string] = [k, session.kind(k) === 'draft' && session.portWired(k, port, output, scope) ? NEW_PORT : port]
      if (output) session.connect(own, other, scope)
      else session.connect(other, own, scope)
    })
  }

  /** Moves a boundary bar; the wires to its ports follow while dragging. */
  const onBoundaryDown = (input: boolean, origin: Pt, e: React.PointerEvent) => {
    if (e.button !== 0) return
    e.stopPropagation()
    focus()
    const start = { x: e.clientX, y: e.clientY }
    drive(e, 3, (ev) => {
      clearHover()
      setBoundaryDrag({ input, dx: (ev.clientX - start.x) / z, dy: (ev.clientY - start.y) / z })
    }, (ev, moved) => {
      setBoundaryDrag(undefined)
      if (!moved) return
      let x = origin.x + (ev.clientX - start.x) / z, y = origin.y + (ev.clientY - start.y) / z
      if (prefs.showGrid) {
        x = snapToGrid(x, prefs.gridSize)
        y = snapToGrid(y, prefs.gridSize)
      }
      // Layout only (the external's layout.json).
      session.setBoundaryPosition(scope, input ? 'inputs' : 'outputs', { x: Math.round(x), y: Math.round(y) })
    })
  }

  const onDrop = (e: React.DragEvent) => {
    const item = e.dataTransfer.getData('text/plain')
    if (!item || !session.isEditable) return
    e.preventDefault()
    const at = cv.world(local(e))
    if (item.startsWith(TEMPLATE_DRAG)) {
      const t = item.slice(TEMPLATE_DRAG.length) as DefinitionTemplate
      if (TEMPLATES.some((x) => x.id === t)) tab.setSelection([session.addCard(t, scope, at)])
    } else if (item.startsWith(EXTERNAL_DEFINITION_DRAG)) {
      const parts = item.slice(EXTERNAL_DEFINITION_DRAG.length).split('\n')
      if (parts.length === 2) void session.placeDefinitionCopy(parts[0], parts[1], scope, at).then((k) => k && tab.setSelection([k]))
    } else if (item.startsWith(EXTERNAL_DRAG)) {
      void session.placeExternalCopy(item.slice(EXTERNAL_DRAG.length), scope, at).then((k) => k && tab.setSelection([k]))
    } else if (item.startsWith(DEFINITION_DRAG)) {
      tab.setSelection([session.placeDefinition(item.slice(DEFINITION_DRAG.length), at)])
    }
  }

  const backgroundMenu = (e: React.MouseEvent) => {
    const at = cv.world(local(e))
    showMenu(e, [
      { label: L('Card を追加'), disabled: !session.isEditable, submenu: builtinCardItems((t) => addBuiltin(t, at)) },
      { label: L('付箋を追加'), action: () => cv.addNote(at) },
      'divider',
      { label: L('全体表示'), action: () => cv.fitAll() },
      { label: L('整列'), action: () => session.arrange(scope) },
      tab.selection.size > 0 && { label: L('選択をグループ化'), disabled: !session.connected, action: () => cv.groupSelection() },
    ])
  }

  // ---- Layers ----

  const b = scene.boundary
  const wires = scene.wires
  const flow = scene.flow
  const sel = tab.selection
  const selectedMid = tab.selectedConnection !== undefined ? midPoint(wires.find((w) => w.connection === tab.selectedConnection)?.pts ?? []) : undefined
  const shown: HoverTarget | undefined = hover ?? (cv.details !== undefined && session.instance(cv.details) ? { kind: 'card', k: cv.details } : undefined)
  let bubbleAt = hoverAt
  if (!hover && shown?.kind === 'card') {
    const r = scene.cardRect(shown.k)
    bubbleAt = screen({ x: r.x + r.width, y: r.y })
  }

  return (
    <div ref={root} className={`cv-canvas${spaceHeld || panning ? ' cv-grab' : ''}`} tabIndex={0} aria-label="Canvas" data-canvas-zoom={z}
      onKeyDown={onKeyDown}
      onKeyUp={(e) => e.key === ' ' && setSpaceHeld(false)}
      onFocus={() => (cv.focused = true)}
      onBlur={(e) => {
        if (!root.current?.contains(e.relatedTarget as Node)) {
          cv.focused = false
          setSpaceHeld(false)
        }
      }}
      onPointerDownCapture={onPointerDownCapture}
      onPointerMove={(e) => root.current?.contains(e.target as Node) && track(local(e))}
      onPointerLeave={clearHover}
      onDragOver={(e) => {
        if (session.isEditable && e.dataTransfer.types.includes('text/plain')) {
          e.preventDefault()
          e.dataTransfer.dropEffect = 'copy'
        }
      }}
      onDrop={onDrop}>
      <div className="cv-bg" style={gridStyle(cam, prefs.showGrid, prefs.gridSize)}
        onPointerDown={onBackgroundDown} onDoubleClick={() => cv.fitAll()} onContextMenu={backgroundMenu} />

      {session.groups(scope).map((g) => {
        const id = str(g.get('id'))
        if (!id) return null
        const members = scene.groupMembers(g)
        const r = scene.groupRect(members)
        return (
          <GroupFrameView key={id} session={session} group={g} scope={scope} zoom={z}
            style={placed(screen(r), r.width * z, r.height * z)}
            onPointerDown={(e) => {
              if (e.button !== 0) return
              e.stopPropagation()
              focus()
              const start = { x: e.clientX, y: e.clientY, cx: cam.x, cy: cam.y }
              drive(e, 2, (ev) => {
                setPanning(true)
                cv.setCamera({ ...cv.camera, x: start.cx + ev.clientX - start.x, y: start.cy + ev.clientY - start.y }, false)
              }, (_ev, moved) => {
                setPanning(false)
                if (moved) cv.saveView()
                else {
                  tab.selectedConnection = undefined
                  tab.setSelection(members)
                }
              })
            }}
            onDoubleClick={(e) => {
              e.stopPropagation()
              cv.set('editingGroup', id)
            }}
            onContextMenu={(e) => showMenu(e, [
              { label: L('グループを編集…'), action: () => cv.set('editingGroup', id) },
              { label: L('メンバーを選択'), action: () => tab.setSelection(members) },
              'divider',
              { label: L('グループを解除（Card は残ります）'), action: () => session.ungroup(id) },
            ])} />
        )
      })}

      <WireLayer wires={wires.map((w) => ({ d: pathToScreen(w.d, cam), style: w.style, dotted: w.dotted }))}
        animate={prefs.animateLines && session.runActive} zoom={z} width={cv.size.width} height={cv.size.height} />

      {tab.showNixReferences && <svg width={cv.size.width} height={cv.size.height} style={{ position: 'absolute', inset: 0, pointerEvents: 'none', overflow: 'visible' }}>
        <defs><marker id={`nix-arrow-${tab.id}`} viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="context-stroke" /></marker></defs>
        {nixLinks.map(ref => {
          const d = pathToScreen(nixCurve(scene.cardRect(ref.source), scene.cardRect(ref.target)), cam)
          const active = activeNix.includes(ref)
          return <g key={ref.id}>
            <path d={d} fill="none" stroke={ref.valid ? 'var(--purple)' : 'var(--orange)'} strokeWidth={active ? 3 : 1.5} strokeDasharray="7 4"
              opacity={tab.hoveredNixCard && !active ? 0.2 : 0.9} markerEnd={`url(#nix-arrow-${tab.id})`} />
            <path d={d} fill="none" stroke="transparent" strokeWidth={14} style={{ pointerEvents: 'stroke', cursor: 'pointer' }}
              role="button" tabIndex={0} aria-label={`${ref.source} → ${ref.target}: ${ref.expression}`}
              onPointerDown={e => e.stopPropagation()}
              onClick={e => { e.stopPropagation(); tab.selectNixReference(ref.id) }}
              onKeyDown={e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); e.stopPropagation(); tab.selectNixReference(ref.id) } }}>
              <title>{ref.expression}</title>
            </path>
          </g>
        })}
        {[...highlightedNix].map(k => {
          const r = scene.cardRect(k), p = screen(r)
          return <rect key={k} x={p.x - 3} y={p.y - 3} width={r.width * z + 6} height={r.height * z + 6} rx={10} fill="none" stroke="var(--purple)" strokeWidth={3} />
        })}
      </svg>}

      {b && <BoundaryBars b={b} zoom={z} screen={screen} drag={boundaryDrag} onDown={onBoundaryDown} />}

      {scene.cardKeys.map((k) => {
        const data = scene.data(k)
        const g = scene.geometry(k)
        return (
          <CardNodeView key={k} data={data} flow={flow} zoom={z} selected={sel.has(k)} guide={'card:' + k}
            style={placed(screen(scene.origin(k)), g.size.width * z, g.size.height * z, data.draftEditing ? { zIndex: 1 } : undefined)}
            onParamCommit={data.kind === 'input' || data.kind === 'switch' ? (name, text) => commitParam(k, name, text) : undefined}
            paramsEditable={session.isEditable}
            onPortDown={session.isEditable ? (port, output, e) => onPortDown(k, port, output, e) : undefined}
            draftEdit={session.inlineDraft?.card === k ? {
              draft: session.inlineDraft,
              nameProblem: session.draftNameProblem(k, session.inlineDraft.name),
              onChange: (change) => session.setDraftEdit(change),
              onDone: (byKey) => {
                finishDraftEdit()
                if (byKey) focus()
              },
            } : undefined}
            onStageTap={(st) => showLog(workspace, k, st, session.cardStates.get(k)?.invocation)}
            onRun={data.kind === 'trigger' && session.connected && !session.runActive ? () => runCard(session, k, true) : undefined}
            onToggleCollapse={() => session.setCollapsed([k], !session.collapsed(k))}
            onComments={() => app.openWindow(session, { kind: 'card', card: k, category: 'comments' })}
            onPointerDown={(e) => onCardDown(k, e)}
            onDoubleClick={(e) => {
              e.stopPropagation()
              openCard(session, k, tab)
            }}
            onContextMenu={(e) => showMenu(e, cardMenu(workspace, session, tab, k, () => cv.groupSelection()))} />
        )
      })}

      {scene.outputKeys.map((k) => {
        const r = scene.frameRect(k)
        const d = frameDrag?.card === k ? frameDrag : undefined
        const p = screen(r)
        return (
          <OutputFrameView key={'f:' + k} session={session} cardKey={k} zoom={z} selected={tab.selectedFrames.has(k)}
            style={placed({ x: p.x + (d?.dx ?? 0), y: p.y + (d?.dy ?? 0) }, r.width * z, r.height * z)}
            onExpand={() => app.openWindow(session, { kind: 'visual', card: k })}
            onClose={() => cv.closeFrame(k)}
            onResize={(dx, dy) => session.updateDisplayFrame(k, (f) => {
              f.set('x', real(Math.round(r.x)))
              f.set('y', real(Math.round(r.y)))
              f.set('width', real(Math.round(Math.max(180, r.width + dx / z))))
              f.set('height', real(Math.round(Math.max(100, r.height + dy / z))))
            })}
            onPointerDown={(e) => {
              if (e.button !== 0) return
              e.stopPropagation()
              focus()
              if (tab.selectedFrames.has(k)) return dragSelection(e)
              const start = { x: e.clientX, y: e.clientY }
              let first = true
              // Follows the pointer while dragged; the layout changes once, on release.
              drive(e, 3, (ev) => {
                if (first) tab.setSelection([])
                first = false
                setFrameDrag({ card: k, dx: ev.clientX - start.x, dy: ev.clientY - start.y })
              }, (ev, moved) => {
                setFrameDrag(undefined)
                if (!moved) return
                session.updateDisplayFrame(k, (f) => {
                  f.set('x', real(Math.round(r.x + (ev.clientX - start.x) / z)))
                  f.set('y', real(Math.round(r.y + (ev.clientY - start.y) / z)))
                  f.set('width', real(r.width))
                  f.set('height', real(r.height))
                })
              })
            }}
            onContextMenu={(e) => showMenu(e, [
              { label: L('拡大して表示'), action: () => app.openWindow(session, { kind: 'visual', card: k }) },
              { label: L('チャットに参照を追加'), action: () => app.chat(session).addReference({ kind: 'display', card: k }) },
              'divider',
              { label: L('枠を削除（Card は残ります）'), action: () => cv.closeFrame(k) },
            ])} />
        )
      })}

      {/* The link's ports, where the link meets the Card and the frame. */}
      {wires.filter((w) => w.style === 'display').flatMap((w, i) => linkEnds(w.d).map((p, j) => {
        const s = screen(p)
        return <span key={`l${i}:${j}`} className="cv-link-dot" style={{ left: s.x - 3.5, top: s.y - 3.5 }} />
      }))}

      {session.notes(scope).map((n) => {
        const id = str(get(n, 'id'))
        if (!id) return null
        const r = noteRect(n)
        const d = noteDrag?.id === id ? noteDrag : cardDrag && tab.selectedNotes.has(id) ? { dx: cardDrag.dx * z, dy: cardDrag.dy * z } : undefined
        const p = screen(r)
        return (
          <StickyNoteView key={id} note={n} zoom={z} selected={tab.selectedNotes.has(id)}
            style={placed({ x: p.x + (d?.dx ?? 0), y: p.y + (d?.dy ?? 0) }, r.width * z, r.height * z)}
            onResize={(dx, dy) => session.updateNote(id, (v) => {
              v.set('width', real(Math.round(Math.max(120, r.width + dx / z))))
              v.set('height', real(Math.round(Math.max(60, r.height + dy / z))))
            })}
            onPointerDown={(e) => {
              if (e.button !== 0) return
              e.stopPropagation()
              focus()
              if (tab.selectedNotes.has(id)) return dragSelection(e)
              const start = { x: e.clientX, y: e.clientY }
              let first = true
              drive(e, 3, (ev) => {
                if (first) tab.setSelection([])
                first = false
                setNoteDrag({ id, dx: ev.clientX - start.x, dy: ev.clientY - start.y })
              }, (ev, moved) => {
                setNoteDrag(undefined)
                if (!moved) return
                session.updateNote(id, (v) => {
                  v.set('x', real(Math.round(r.x + (ev.clientX - start.x) / z)))
                  v.set('y', real(Math.round(r.y + (ev.clientY - start.y) / z)))
                })
              })
            }}
            onDoubleClick={(e) => {
              e.stopPropagation()
              cv.set('editingNote', id)
            }}
            onContextMenu={(e) => {
              const current = noteFontSize(n)
              const setFont = (v: number) => {
                const f = Math.max(8, Math.min(48, v))
                session.updateNote(id, (x) => x.set('fontSize', f === 13 ? undefined : real(f)))
              }
              showMenu(e, [
                { label: L('付箋を編集…'), action: () => cv.set('editingNote', id) },
                { label: L('色'), submenu: NOTE_COLORS.map((c) => ({ label: c.label, checked: (str(get(n, 'color')) ?? 'yellow') === c.id, action: () => session.updateNote(id, (x) => x.set('color', c.id)) })) },
                {
                  label: L('文字の大きさ'), submenu: [
                    { label: L('大きく'), action: () => setFont(current + 2) },
                    { label: L('小さく'), action: () => setFont(current - 2) },
                    'divider',
                    ...[11, 13, 16, 20, 28].map((v) => ({ label: L('{0}{1}', v, v === 13 ? L('（デフォルト）') : ''), checked: current === v, action: () => setFont(v) })),
                  ],
                },
                'divider',
                { label: L('付箋を削除'), destructive: true, action: () => session.removeNote(id) },
              ])
            }} />
        )
      })}

      <WritingPanels writers={app.chat(session).writers} team={app.chat(session).team} known={(k) => scene.cardKeys.includes(k)} rect={(k) => scene.outlineRect(k)}
        obstacles={() => [...scene.cardKeys.map((k) => scene.cardRect(k)), ...scene.outputKeys.map((k) => scene.frameRect(k))]}
        screen={screen} zoom={z} size={cv.size} />
      <HelperBirds chat={app.chat(session)} known={(k) => scene.cardKeys.includes(k)} rect={(k) => scene.outlineRect(k)} screen={screen} zoom={z} size={cv.size} />

      {pending && (() => {
        const a = screen(scene.portWorld(pending.from[0], pending.from[1], pending.fromOutput))
        return (
          <svg className="cv-wires" width={cv.size.width} height={cv.size.height} aria-hidden>
            <path d={`M${a.x},${a.y}L${pending.point.x},${pending.point.y}`} fill="none" stroke="var(--accent)" strokeWidth={2} strokeDasharray="6 4" />
          </svg>
        )
      })()}

      {marquee && (
        <div className="cv-marquee" style={{
          left: Math.min(marquee.start.x, marquee.end.x), top: Math.min(marquee.start.y, marquee.end.y),
          width: Math.abs(marquee.end.x - marquee.start.x), height: Math.abs(marquee.end.y - marquee.start.y),
        }} />
      )}

      {selectedMid && tab.selectedConnection !== undefined && (() => {
        const m = screen(selectedMid)
        const i = tab.selectedConnection
        return (
          <div className="cv-cut" style={{ left: m.x, top: m.y }} onPointerDown={(e) => e.stopPropagation()}>
            <Button kind="prominent" small icon="scissors" disabled={!session.isEditable}
              onClick={() => {
                if (!session.isEditable) return
                session.removeConnection(i, scope)
                tab.selectedConnection = undefined
                tab.changed()
              }}>{L('切断')}</Button>
          </div>
        )
      })()}

      {shown && !interacting && (
        <div ref={bubble} className="cv-bubble-host" style={bubblePosition(bubbleAt, bubbleSize, cv.size)}>
          <HoverBubble content={hoverContent(session, scope, shown)} />
        </div>
      )}

      {cv.findText !== undefined && (
        <FindBox session={session} keys={scene.cardKeys} text={cv.findText} field={findField}
          onChange={(t) => cv.set('findText', t)}
          onClose={() => {
            cv.set('findText', undefined)
            focus()
          }}
          onPick={(k) => {
            tab.setSelection([k])
            cv.set('findText', undefined)
            cv.reveal(k)
            focus()
          }} />
      )}

      {cv.editingNote && session.note(cv.editingNote) && (
        <NoteEditor session={session} noteId={cv.editingNote} onClose={() => cv.set('editingNote', undefined)} />
      )}
      {cv.editingGroup && session.groups(scope).some((g) => str(g.get('id')) === cv.editingGroup) && (
        <GroupEditor session={session} scope={scope} groupId={cv.editingGroup} onClose={() => cv.set('editingGroup', undefined)} />
      )}
    </div>
  )
}


/** Right below the pointer (its top-left corner just past it), flipped to the
 *  other side where the canvas ends, and kept inside the canvas. */
function bubblePosition(at: Pt, size: { width: number; height: number }, view: { width: number; height: number }): CSSProperties {
  const gap = 14
  let left = at.x + gap
  if (left + size.width > view.width - 6) left = at.x - gap - size.width
  let top = at.y + gap
  if (top + size.height > view.height - 6) top = at.y - gap - size.height
  left = Math.min(Math.max(6, left), Math.max(6, view.width - size.width - 6))
  top = Math.min(Math.max(6, top), Math.max(6, view.height - size.height - 6))
  return { left, top }
}

// ---- Menus ------------------------------------------------------------------------------

/** The built-in Cards, for the canvas menu and the + button. */
export function builtinCardItems(add: (t: DefinitionTemplate) => void): MenuItem[] {
  const item = (id: DefinitionTemplate): MenuItem => ({ label: TEMPLATES.find((t) => t.id === id)?.label ?? id, action: () => add(id) })
  const visualization: MenuItem = {
    label: L('可視化'),
    submenu: visualizationTemplates.map(t => ({ label: t.label, action: () => add(t.id) })),
  }
  return [item('python'), item('cpp'), item('csharp'), item('draft'), visualization, 'divider', item('trigger'), item('input'), item('switch'), item('fork'), item('branch'), item('merge'), item('logger')]
}
export { templateIcon }

/** Right-click menu of a Card. */
export function cardMenu(workspace: Workspace, session: ProjectSession, tab: WorkflowTab, k: CardKey, onGroup?: () => void): MenuItem[] {
  const kind = session.kind(k)
  const targets = tab.selection.has(k) ? new Set(tab.selection) : new Set([k])
  const cannotRun = !session.connected || session.runActive
  const editable = session.isEditable
  return [
    ...(kind === 'subgraph' ? [
      { label: L('開く'), action: () => tab.enter(path(k)) },
      { label: L('新しいタブで開く'), action: () => workspace.openTab(path(k)) },
      { label: L('別ウインドウで開く'), action: () => openWorkflowWindow(session, path(k)) },
      'divider' as const,
    ] : []),
    kind === 'draft' && { label: L('名前と内容を書く'), disabled: !editable, action: () => canvasFor(session, tab).writeDraft(k) },
    { label: L('設定…'), action: () => app.openWindow(session, { kind: 'card', card: k }) },
    { label: L('コメント…'), action: () => app.openWindow(session, { kind: 'card', card: k, category: 'comments' }) },
    { label: L('実行履歴…'), action: () => app.openWindow(session, { kind: 'executions', card: k }) },
    { label: L('成果物…'), action: () => app.openWindow(session, { kind: 'artifacts', card: k }) },
    { label: L('ログを表示'), action: () => showLog(workspace, k) },
    {
      label: L('チャットに参照を追加'), action: () => {
        for (const t of [...targets].sort()) app.chat(session).addReference({ kind: 'card', card: t })
        workspace.set('showChat', true)
      },
    },
    'divider',
    { label: L('ここから下流を実行'), disabled: cannotRun, action: () => runCard(session, k, true) },
    { label: L('この Card だけ実行（最後の入力）'), disabled: cannotRun, action: () => runCard(session, k, false) },
    'divider',
    { label: L('複製'), disabled: !editable, action: () => void session.duplicate(targets, tab.scope).then((s) => tab.setSelection(s)) },
    { label: session.collapsed(k) ? L('展開') : L('折りたたむ'), action: () => session.setCollapsed(targets, !session.collapsed(k)) },
    get(session.displayFrame(k), 'hidden') === true && {
      label: L('出力表示を戻す'), action: () => session.updateDisplayFrame(k, (f) => f.set('hidden', undefined)),
    },
    {
      label: L('グループ化（見た目だけ）'), action: () => {
        if (!tab.selection.has(k)) tab.setSelection([k])
        if (onGroup) onGroup()
        else canvasFor(session, tab).groupSelection()
      },
    },
    {
      label: L('Card in Card にまとめる（新しい external プロジェクト）'), disabled: !editable,
      action: () => void session.makeSubgraph(targets, tab.scope).then((nk) => nk && tab.setSelection([nk])),
    },
    'divider',
    { label: L('入力を切断'), disabled: !editable, action: () => session.disconnect(k, true, tab.scope) },
    { label: L('出力を切断'), disabled: !editable, action: () => session.disconnect(k, false, tab.scope) },
    {
      label: L('削除'), destructive: true, disabled: !editable, action: () => {
        session.deleteCards(targets, tab.scope)
        tab.setSelection([...tab.selection].filter((x) => !targets.has(x)))
      },
    },
  ]
}

// ---- Card in Card boundary ------------------------------------------------------------------

function BoundaryBars({ b, zoom, screen, drag, onDown }: {
  b: Boundary; zoom: number; screen: (p: Pt) => Pt; drag?: SceneDrag['boundary']; onDown: (input: boolean, origin: Pt, e: React.PointerEvent) => void
}) {
  /** A bar's position without the drag in progress. */
  const undragged = (r: Rect, input: boolean): Pt => (drag && drag.input === input ? { x: r.x - drag.dx, y: r.y - drag.dy } : { x: r.x, y: r.y })
  return (
    <>
      {([[b.top, b.inputs, true], [b.bottom, b.outputs, false]] as const).map(([rect, ports, input]) => (
        <BoundaryBarView key={input ? 'in' : 'out'} title={input ? L('この Card の入力') : L('この Card の出力')} ports={ports} input={input} zoom={zoom} rect={rect}
          style={placed(screen(rect), rect.width * zoom, rect.height * zoom)}
          onPointerDown={(e) => onDown(input, undragged(rect, input), e)} />
      ))}
    </>
  )
}

// ---- Groups, notes, output frames ------------------------------------------------------------

type ItemEvents = {
  style: CSSProperties
  onPointerDown?: (e: React.PointerEvent) => void
  onDoubleClick?: (e: React.MouseEvent) => void
  onContextMenu?: (e: React.MouseEvent) => void
}

/** Visual grouping frame drawn behind its Cards. */
function GroupFrameView({ session, group, scope, zoom: z, ...ev }: { session: ProjectSession; group: JObj; scope: string[]; zoom: number } & ItemEvents) {
  const color = noteColor(group.get('color'))
  const outputs = (arr(group.get('outputs')) ?? []).flatMap((o) => {
    const c = str(get(o, 'card')), p = str(get(o, 'port'))
    return c && p ? [`${session.displayName(key([...scope, c]))}.${p}`] : []
  })
  const name = str(group.get('name'))
  return (
    <div className="cv-group" {...ev} aria-label={L('グループ {0}', name ?? '')}
      style={{ ...ev.style, borderRadius: 12 * z, background: tint(color, 0.07), borderColor: tint(color, 0.5) }}>
      <div className="cv-group-title" style={{ color, gap: 6 * z, padding: `${5 * z}px ${10 * z}px 0` }}>
        <Icon name="rectangle.3.group" size={10 * z} />
        <span style={{ fontSize: 12 * z, fontWeight: 600 }}>{name ?? L('グループ')}</span>
      </div>
      {outputs.length > 0 && (
        <div className="cv-group-outputs" style={{ gap: 2 * z, padding: 6 * z }} title={L('グループの公開出力')}>
          {outputs.map((o) => (
            <span key={o} className="cv-group-output" style={{ color, fontSize: 10 * z, padding: `${2 * z}px ${6 * z}px`, background: tint(color, 0.15), gap: 3 * z }}>
              <Icon name="arrow.right.circle" size={10 * z} />{o}
            </span>
          ))}
        </div>
      )}
    </div>
  )
}

export const noteFontSize = (n: J | undefined) => dbl(get(n, 'fontSize')) ?? 13

/** Markdown sticky note; it does not take part in runs. Its text size is its own (`fontSize` in layout.json). */
function StickyNoteView({ note, zoom: z, selected, onResize, ...ev }: { note: J; zoom: number; selected: boolean; onResize: (dx: number, dy: number) => void } & ItemEvents) {
  const color = noteColor(get(note, 'color'))
  return (
    <div className={`cv-note${selected ? ' selected' : ''}`} {...ev} title={L('ダブルクリックで編集')}
      style={{ ...ev.style, padding: 10 * z, borderRadius: 6 * z, background: tint(color, 0.28), borderColor: tint(color, 0.5) }}>
      {/* Cut at the note's size (no scroll bar on the canvas; resize to see more). */}
      <div className="cv-note-text" style={{ fontSize: noteFontSize(note) * z }}><Markdown text={str(get(note, 'text')) ?? ''} /></div>
      <ResizeHandle onEnd={onResize} />
    </div>
  )
}

function ResizeHandle({ onEnd }: { onEnd: (dx: number, dy: number) => void }) {
  const [live, setLive] = useState({ x: 0, y: 0 })
  return (
    <span className="cv-resize secondary" title={L('ドラッグしてサイズ変更')} style={{ transform: `translate(${live.x}px, ${live.y}px)` }}
      onDoubleClick={(e) => e.stopPropagation()}
      onPointerDown={(e) => {
        if (e.button !== 0) return
        e.stopPropagation()
        const sx = e.clientX, sy = e.clientY
        drive(e, 1, (ev) => setLive({ x: ev.clientX - sx, y: ev.clientY - sy }), (ev, moved) => {
          setLive({ x: 0, y: 0 })
          if (moved) onEnd(ev.clientX - sx, ev.clientY - sy)
        })
      }}>
      <Icon name="arrow.down.right.and.arrow.up.left" size={8} />
    </span>
  )
}

/** A Card's result in its own movable frame. */
function OutputFrameView({ session, cardKey, zoom: z, selected, onExpand, onClose, onResize, ...ev }: {
  session: ProjectSession; cardKey: CardKey; zoom: number; selected: boolean; onExpand: () => void; onClose: () => void; onResize: (dx: number, dy: number) => void
} & ItemEvents) {
  const figure = session.figureState(cardKey)
  const latest = session.displays.get(cardKey)
  const hold = holdsDisplay(figure)
  const settled = useRef(hold ? undefined : latest)
  useLayoutEffect(() => { if (!hold) settled.current = latest }, [hold, latest])
  const d = hold ? settled.current : latest
  const stop = (e: React.PointerEvent | React.MouseEvent) => e.stopPropagation()
  return (
    <div className={`cv-frame${selected ? ' selected' : ''}`} {...ev} style={{ ...ev.style, padding: 8 * z, borderRadius: 8 * z, gap: 4 * z }}>
      <div className="cv-frame-header secondary" style={{ gap: 6 * z }}>
        <Icon name="chart.xyaxis.line" size={10 * z} style={{ color: 'var(--purple)' }} />
        <span className="cv-ellipsis" style={{ fontSize: 10 * z, fontWeight: 700, color: 'var(--label)' }}>{session.displayName(cardKey)}</span>
        {d && (
          <span className="mono-digits nowrap" style={{ fontSize: 9 * z }} title={L('Run {0} の中で、この Card の {1} 回目の実行の結果', d.runId, d.invocation)}>
            {L('Run {0} · 実行 #{1}', d.runId.slice(0, 6), d.invocation)}
          </span>
        )}
        <span style={{ flex: 1 }} />
        <button type="button" className="cv-icon-button" title={L('拡大して表示')} onPointerDown={stop} onDoubleClick={stop} onClick={(e) => { e.stopPropagation(); onExpand() }}>
          <Icon name="arrow.up.left.and.arrow.down.right" size={11 * z} />
        </button>
        <button type="button" className="cv-icon-button" title={L('枠を削除（Card は残ります）')} onPointerDown={stop} onDoubleClick={stop} onClick={(e) => { e.stopPropagation(); onClose() }}>
          <Icon name="xmark" size={11 * z} />
        </button>
      </div>
      <div className="cv-frame-body" aria-busy={hold} style={{ opacity: hold ? 0.4 : 1 }}>
        <DisplayContent display={d} zoom={z} emptyNote={session.noResultNote(cardKey)} figure={figure.mode === 'live' ? figure : undefined} />
      </div>
      <ResizeHandle onEnd={onResize} />
    </div>
  )
}

// ---- Hover details -------------------------------------------------------------------------

interface BubbleContent {
  title: string
  subtitle: string
  symbol: string
  lines?: string[]
  status?: string
  problems?: string[]
  warnings?: string[]
}

function hoverContent(session: ProjectSession, scope: string[], t: HoverTarget): BubbleContent {
  if (t.kind === 'card') {
    const d = session.nodeData(t.k)
    const lines = session.cardDetails(t.k)
    let status = runText(d) ?? idleText(d)
    const id = session.currentRunId
    if (id && d.invocation > 0n) status += L('（Run {0} の中でこの Card の {1} 回目の実行）', id.slice(0, 8), d.invocation)
    return {
      title: d.title, subtitle: KIND_LABEL[d.kind], symbol: d.symbol ?? KIND_ICON[d.kind], lines, status,
      problems: [...d.errors, ...(d.failure ? [d.failure] : [])], warnings: d.warnings,
    }
  }
  const symbol = 'point.topleft.down.to.point.bottomright.curvepath'
  const c = session.connections(scope)[t.i]
  const fc = str(get(c, 'from', 'card')), fp = str(get(c, 'from', 'port')), tc = str(get(c, 'to', 'card')), tp = str(get(c, 'to', 'port'))
  if (!fc || !fp || !tc || !tp) return { title: L('接続'), subtitle: '', symbol }
  const fk = key([...scope, fc]), tk = key([...scope, tc])
  const out = session.resolution.get(fk)?.outputs.find((p) => p.name === fp)
  const inp = session.resolution.get(tk)?.inputs.find((p) => p.name === tp)
  const lines = [`${session.displayName(fk)}.${fp}  →  ${session.displayName(tk)}.${tp}`]
  lines.push(L('型: ') + (out?.rootType ?? inp?.rootType ?? L('未解決')))
  lines.push(inp?.optional ? L('受け側の入力: 省略可') : L('受け側の入力: 必須'))
  let status = L('この Run ではまだ値が通っていません')
  const st = session.cardStates.get(fk)
  const id = session.currentRunId
  if (st && st.sentInRun.has(fp) && id) status = L('Run {0} で値が通りました（送信元の最新は実行 #{1}）', id.slice(0, 8), st.invocation)
  return { title: L('接続'), subtitle: '', symbol, lines, status }
}

/** Details shown near the pointer over a Card or a wire. */
function HoverBubble({ content }: { content: BubbleContent }) {
  return (
    <div className="cv-bubble material">
      <div className="row gap6" style={{ alignItems: 'center' }}>
        <Icon name={content.symbol} className="secondary" />
        <span className="cv-ellipsis" style={{ fontWeight: 600, fontSize: 12 }}>{content.title}</span>
        {content.subtitle && <span className="caption secondary nowrap">{content.subtitle}</span>}
      </div>
      {content.lines?.map((l, i) => <div key={i} className="caption wrap-pre cv-clamp8">{l}</div>)}
      {content.status && <div className="caption secondary">{content.status}</div>}
      {content.problems?.map((p, i) => (
        <div key={'p' + i} className="caption cv-issue" style={{ color: 'var(--red)' }}><Icon name="xmark.octagon" size={11} /><span className="clamp3">{p}</span></div>
      ))}
      {content.warnings?.map((p, i) => (
        <div key={'w' + i} className="caption cv-issue" style={{ color: 'var(--orange)' }}><Icon name="exclamationmark.triangle" size={11} /><span className="clamp3">{p}</span></div>
      ))}
    </div>
  )
}

// ---- Card search (⌘F) -------------------------------------------------------------------------

function FindBox({ session, keys, text, field, onChange, onClose, onPick }: {
  session: ProjectSession; keys: CardKey[]; text: string; field: React.RefObject<HTMLInputElement & HTMLTextAreaElement>
  onChange: (t: string) => void; onClose: () => void; onPick: (k: CardKey) => void
}) {
  const q = text.trim().toLowerCase()
  const matches = q ? keys.filter((k) => session.displayName(k).toLowerCase().includes(q) || k.toLowerCase().includes(q)
    || (session.cardDescription(k) ?? '').toLowerCase().includes(q)) : []
  return (
    <div className="cv-find material" onPointerDown={(e) => e.stopPropagation()} onDoubleClick={(e) => e.stopPropagation()}>
      <div className="row gap6" style={{ alignItems: 'center' }}>
        <Icon name="magnifyingglass" className="secondary" />
        <input ref={field} className="cv-find-field" value={text} placeholder={L('Card を検索（名前・説明）')} spellCheck={false}
          onChange={(e) => onChange(e.target.value)}
          onKeyDown={(e) => {
            e.stopPropagation()
            if (e.key === 'Enter' && !e.nativeEvent.isComposing && matches[0]) onPick(matches[0])
            if (e.key === 'Escape') onClose()
          }} />
        <button type="button" className="cv-icon-button secondary" aria-label={L('閉じる')} onClick={onClose}><Icon name="xmark.circle.fill" /></button>
      </div>
      {matches.slice(0, 8).map((k) => (
        <button key={k} type="button" className="cv-find-item" onClick={() => onPick(k)}>
          <Icon name={kindIcon(session.kind(k))} />
          <span className="flex1 cv-ellipsis" style={{ textAlign: 'left' }}>{session.displayName(k)}</span>
          <span className="caption secondary">{KIND_LABEL[session.kind(k)]}</span>
        </button>
      ))}
      {q !== '' && !matches.length && <div className="caption secondary">{L('見つかりません')}</div>}
    </div>
  )
}

// ---- Editors -------------------------------------------------------------------------------

function ColorPicker({ value, onChange }: { value: string; onChange: (c: string) => void }) {
  return (
    <div className="row gap8" style={{ alignItems: 'center' }}>
      <span>{L('色')}</span>
      <Segmented value={value} options={NOTE_COLORS.map((c) => ({ value: c.id, label: c.label }))} onChange={onChange} />
    </div>
  )
}

function EditorButtons({ remove, removeLabel, onClose, onApply }: { remove: () => void; removeLabel: string; onClose: () => void; onApply: () => void }) {
  return (
    <div className="row gap8" style={{ alignItems: 'center' }}>
      <Button kind="destructive" onClick={() => { remove(); onClose() }}>{removeLabel}</Button>
      <span className="flex1" />
      <Button onClick={onClose}>{L('キャンセル')}</Button>
      <Button kind="prominent" type="submit">{L('適用')}</Button>
    </div>
  )
}

function EditorSheet({ title, width, onClose, onApply, children }: { title: string; width: number; onClose: () => void; onApply: () => void; children: ReactNode }) {
  return (
    <Sheet onClose={onClose} width={width}>
      <form className="sheet-body" onSubmit={(e) => { e.preventDefault(); onApply(); onClose() }}
        onKeyDown={(e) => {
          if (e.key === 'Enter' && (e.metaKey || e.ctrlKey)) {
            e.preventDefault()
            onApply()
            onClose()
          }
        }}>
        <div className="title2">{title}</div>
        {children}
      </form>
    </Sheet>
  )
}

/** Edits a sticky note: Markdown text, color, size. */
function NoteEditor({ session, noteId, onClose }: { session: ProjectSession; noteId: string; onClose: () => void }) {
  const [init] = useState(() => session.note(noteId))
  const [text, setText] = useState(str(get(init, 'text')) ?? '')
  const [color, setColor] = useState(str(get(init, 'color')) ?? 'yellow')
  const [width, setWidth] = useState(dbl(get(init, 'width')) ?? 240)
  const [height, setHeight] = useState(dbl(get(init, 'height')) ?? 120)
  const [fontSize, setFontSize] = useState(dbl(get(init, 'fontSize')) ?? 13)
  const apply = () => session.updateNote(noteId, (n) => {
    n.set('text', text)
    n.set('color', color)
    n.set('width', real(width))
    n.set('height', real(height))
    n.set('fontSize', fontSize === 13 ? undefined : real(fontSize))
  })
  return (
    <EditorSheet title={L('付箋を編集')} width={560} onClose={onClose} onApply={apply}>
      <div className="row gap8" style={{ height: 220 }}>
        <TextField multiline mono value={text} onChange={setText} autoFocus style={{ flex: 1, minWidth: 220, height: '100%', resize: 'none', fontSize: 13 }} />
        <div className="cv-note-preview" style={{ background: tint(noteColor(color), 0.2), fontSize: fontSize }}><Markdown text={text} /></div>
      </div>
      <div className="caption secondary">{L('Markdown（見出し・太字・リスト・表・コード）と数式（$…$、$$…$$）を使えます。付箋は実行には参加しません。')}</div>
      <Stepper label={L('文字の大きさ {0}', Math.round(fontSize))} value={fontSize} onChange={setFontSize} min={8} max={48} step={1} />
      <ColorPicker value={color} onChange={setColor} />
      <div className="row gap16">
        <Stepper label={L('幅 {0}', Math.round(width))} value={width} onChange={setWidth} min={120} max={800} step={20} />
        <Stepper label={L('高さ {0}', Math.round(height))} value={height} onChange={setHeight} min={60} max={600} step={20} />
      </div>
      <EditorButtons removeLabel={L('削除')} remove={() => session.removeNote(noteId)} onClose={onClose} onApply={apply} />
    </EditorSheet>
  )
}

/** Edits a group: name, members and the outputs shown on the frame. */
function GroupEditor({ session, scope, groupId, onClose }: { session: ProjectSession; scope: string[]; groupId: string; onClose: () => void }) {
  const [init] = useState(() => session.groups(scope).find((g) => str(g.get('id')) === groupId))
  const [name, setName] = useState(str(init?.get('name')) ?? '')
  const [color, setColor] = useState(str(init?.get('color')) ?? 'blue')
  const [members, setMembers] = useState(() => new Set((arr(init?.get('cards')) ?? []).map((c) => str(c)).filter((x): x is string => !!x)))
  const [outputs, setOutputs] = useState(() => new Set((arr(init?.get('outputs')) ?? []).flatMap((o) => {
    const c = str(get(o, 'card')), p = str(get(o, 'port'))
    return c && p ? [`${c}\t${p}`] : []
  })))
  const ids = session.cards(scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x)
  const flip = (s: Set<string>, v: string, on: boolean) => {
    const n = new Set(s)
    if (on) n.add(v)
    else n.delete(v)
    return n
  }
  const apply = () => session.updateGroup(groupId, (g) => {
    g.set('name', name)
    g.set('color', color)
    g.set('cards', [...members].sort())
    const outs: J[] = [...outputs].sort().flatMap((t) => {
      const [c, p] = t.split('\t')
      return c && p && members.has(c) ? [obj([['card', c], ['port', p]])] : []
    })
    g.set('outputs', outs.length ? outs : undefined)
  })
  return (
    <EditorSheet title={L('グループを編集')} width={600} onClose={onClose} onApply={apply}>
      <TextField value={name} onChange={setName} placeholder={L('名前')} autoFocus />
      <ColorPicker value={color} onChange={setColor} />
      <div className="row gap12" style={{ height: 240 }}>
        <div className="col flex1">
          <div className="headline">{L('所属する Card')}</div>
          <div className="cv-list">
            {ids.map((id) => (
              <Toggle key={id} kind="checkbox" checked={members.has(id)} label={session.displayName(key([...scope, id]))}
                onChange={(v) => setMembers((m) => flip(m, id, v))} />
            ))}
          </div>
        </div>
        <div className="col flex1">
          <div className="headline">{L('公開する出力')}</div>
          <div className="cv-list">
            {[...members].sort().flatMap((id) => session.ports(key([...scope, id])).outputs.map((p) => {
              const tag = `${id}\t${p}`
              return (
                <Toggle key={tag} kind="checkbox" checked={outputs.has(tag)} label={`${session.displayName(key([...scope, id]))}.${p}`}
                  onChange={(v) => setOutputs((o) => flip(o, tag, v))} />
              )
            }))}
          </div>
        </div>
      </div>
      <div className="caption secondary">{L('グループは見た目のまとまりです。実行順序は変わらず、解除しても Card は残ります。')}</div>
      <EditorButtons removeLabel={L('グループを解除')} remove={() => session.ungroup(groupId)} onClose={onClose} onApply={apply} />
    </EditorSheet>
  )
}
