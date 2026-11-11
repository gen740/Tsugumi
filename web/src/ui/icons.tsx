// Line icons standing in for the SF Symbols the macOS UI uses (same names,
// so views read like their SwiftUI counterparts). 16×16 grid, stroked.
import type { CSSProperties } from 'react'
import { ICONS as CANVAS } from './icons/canvas'
import { ICONS as CHAT } from './icons/chat'
import { ICONS as DETAIL } from './icons/detail'
import { ICONS as SHELL } from './icons/shell'

const BASE: Record<string, string> = {
  circle: 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8z',
  'circle.dotted': 'M8 1.8v.01M12.4 3.6v.01M14.2 8v.01M12.4 12.4v.01M8 14.2v.01M3.6 12.4v.01M1.8 8v.01M3.6 3.6v.01',
  'play.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM6.5 5.4v5.2L10.6 8z',
  'pause.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM6.5 5.5v5M9.5 5.5v5',
  'stop.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM5.8 5.8h4.4v4.4H5.8z',
  'info.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM8 7.2v4M8 4.9v.2',
  'arrow.uturn.right.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM5.5 10.5V8.2a2 2 0 0 1 2-2h3.2M9.2 4.7l1.6 1.5-1.6 1.5',
  'arrow.right.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM5 8h6M8.8 5.8 11 8l-2.2 2.2',
  'minus.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM5.2 8h5.6',
  'ellipsis.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM5 8h.01M8 8h.01M11 8h.01',
  'pencil.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM5.4 10.6l.4-1.8 3.8-3.8 1.4 1.4-3.8 3.8z',
  nosign: 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM3.6 3.6l8.8 8.8',
  hourglass: 'M4.5 2h7M4.5 14h7M5.2 2c0 3 5.6 3.4 5.6 6S5.2 11 5.2 14M10.8 2c0 3-5.6 3.4-5.6 6s5.6 3 5.6 6',
  gearshape: 'M8 5.6a2.4 2.4 0 1 0 0 4.8 2.4 2.4 0 0 0 0-4.8zM8 1.5v1.8M8 12.7v1.8M1.5 8h1.8M12.7 8h1.8M3.4 3.4l1.3 1.3M11.3 11.3l1.3 1.3M3.4 12.6l1.3-1.3M11.3 4.7l1.3-1.3',
  'slider.horizontal.3': 'M2 4h12M2 8h12M2 12h12M5 2.6v2.8M10.5 6.6v2.8M7 10.6v2.8',
  'switch.2': 'M4 2h8a2 2 0 0 1 0 4H4a2 2 0 0 1 0-4zM4 10h8a2 2 0 0 1 0 4H4a2 2 0 0 1 0-4zM4 4h.01M12 12h.01',
  'arrow.triangle.branch': 'M4 14V9.5c0-2 1.5-3 4-3.5l4-.8M4 2v7.5M10.4 3.2 12.2 5 10.4 6.8',
  'arrow.triangle.swap': 'M3 5h9.5M10.5 3l2 2-2 2M13 11H3.5M5.5 9l-2 2 2 2',
  'arrow.triangle.merge': 'M8 14V8.5L3.5 3M8 8.5 12.5 3M3.5 5.8V3h2.8M12.5 5.8V3H9.7',
  'arrow.triangle.2.circlepath': 'M13 6.5A5.2 5.2 0 0 0 3.6 4.8M3 9.5a5.2 5.2 0 0 0 9.4 1.7M3.4 2.5v2.6H6M12.6 13.5v-2.6H10',
  'list.bullet.rectangle': 'M2 2.8h12v10.4H2zM5 6h.01M7 6h4.5M5 10h.01M7 10h4.5',
  'list.bullet': 'M3 4h.01M3 8h.01M3 12h.01M5.5 4H13M5.5 8H13M5.5 12H13',
  'pencil.and.outline': 'M3 13l.6-2.4 7.2-7.2 1.8 1.8-7.2 7.2zM9.6 4.6l1.8 1.8M2 15h12',
  'square.and.pencil': 'M13 9v4.5H2.5V3H7M6 10l.4-2 5.4-5.4 1.6 1.6L8 9.6z',
  'square.stack.3d.up': 'M8 2 14 5 8 8 2 5zM2 8l6 3 6-3M2 11l6 3 6-3',
  'square.stack.3d.up.fill': 'M8 2 14 5 8 8 2 5zM2 8l6 3 6-3M2 11l6 3 6-3',
  'square.stack': 'M3 5.5h10v8H3zM4.5 3.5h7M6 1.8h4',
  'square.on.square': 'M5 5h8.5v8.5H5zM3 11V2.5h8.5',
  'chevron.left.forwardslash.chevron.right': 'M5 4.5 1.8 8 5 11.5M11 4.5 14.2 8 11 11.5M9.2 3 6.8 13',
  snowflake: 'M8 1.5v13M2.4 4.8l11.2 6.4M2.4 11.2l11.2-6.4M6.3 2.5 8 4l1.7-1.5M6.3 13.5 8 12l1.7 1.5',
  'doc.text': 'M3.5 1.8h6l3 3v9.4h-9zM9.5 1.8v3h3M5.6 8h4.8M5.6 10.5h4.8',
  doc: 'M3.5 1.8h6l3 3v9.4h-9zM9.5 1.8v3h3',
  'doc.questionmark': 'M3.5 1.8h6l3 3v9.4h-9zM9.5 1.8v3h3M6.6 8a1.4 1.4 0 1 1 2 1.3c-.5.3-.6.6-.6 1M8 12.2v.1',
  'arrow.left.arrow.right': 'M2 5.5h12M11.5 3l2.5 2.5-2.5 2.5M14 10.5H2M4.5 8 2 10.5 4.5 13',
  'clock.arrow.circlepath': 'M2.6 8a5.4 5.4 0 1 0 1.6-3.8M2.4 2.4v2.6H5M8.4 5v3.4l2.2 1.4',
  clock: 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM8 4.5V8l2.4 1.6',
  shippingbox: 'M8 1.8 14 4.6v6.8L8 14.2 2 11.4V4.6zM2 4.6 8 7.4l6-2.8M8 7.4v6.8M5 3.2l6 2.8',
  'text.bubble': 'M2 3h12v8H7l-3.5 2.8V11H2zM4.8 6h6.4M4.8 8.2h4',
  folder: 'M1.8 3.5h4.4l1.4 1.6h6.6v7.9H1.8z',
  'questionmark.folder': 'M1.8 3.5h4.4l1.4 1.6h6.6v7.9H1.8zM6.8 8a1.3 1.3 0 1 1 1.8 1.2c-.4.2-.6.5-.6.9M8 11.6v.1',
  'square.and.arrow.down': 'M5 6.5h-2v7.3h10V6.5h-2M8 1.5v8M5.5 7.2 8 9.7l2.5-2.5',
  'bubble.left.and.text.bubble.right': 'M1.8 2.5h7.5v5.3H5l-2 1.8V7.8H1.8zM10.8 6.5h3.4v5.3h-1.2v1.8l-2-1.8H6.8v-1.6M3.8 4.6h3.5',
  'bubble.left.and.bubble.right.fill': 'M1.8 2.5h7.5v5.3H5l-2 1.8V7.8H1.8zM10.8 6.5h3.4v5.3h-1.2v1.8l-2-1.8H6.8v-1.6',
  'bubble.left': 'M2 3h12v8H7l-3.5 2.8V11H2z',
  xmark: 'M4 4l8 8M12 4l-8 8',
  'xmark.octagon': 'M5.4 1.8h5.2l3.6 3.6v5.2l-3.6 3.6H5.4l-3.6-3.6V5.4zM6 6l4 4M10 6l-4 4',
  plus: 'M8 3v10M3 8h10',
  minus: 'M3 8h10',
  'plus.square': 'M2.2 2.2h11.6v11.6H2.2zM8 5v6M5 8h6',
  'chevron.right': 'M6 3.5 10.5 8 6 12.5',
  'chevron.down': 'M3.5 6 8 10.5 12.5 6',
  'chevron.backward': 'M10 3.5 5.5 8l4.5 4.5',
  'exclamationmark.triangle': 'M8 2 14.5 13.5h-13zM8 6.5v3.2M8 11.6v.1',
  'checkmark.icloud': 'M4.5 12.5a3 3 0 0 1-.4-6A4.2 4.2 0 0 1 12.2 6a3.3 3.3 0 0 1-.2 6.5zM6 9.2l1.5 1.4L10.2 8',
  'exclamationmark.icloud.fill': 'M4.5 12.5a3 3 0 0 1-.4-6A4.2 4.2 0 0 1 12.2 6a3.3 3.3 0 0 1-.2 6.5zM8 6.5v2.6M8 10.6v.1',
  'icloud.slash': 'M4.5 12.5a3 3 0 0 1-.4-6A4.2 4.2 0 0 1 12.2 6a3.3 3.3 0 0 1-.2 6.5zM2 2l12 12',
  'checkmark.seal': 'M8 1.5l1.6 1.2 2-.1.6 1.9 1.6 1.2-.6 1.9.6 1.9-1.6 1.2-.6 1.9-2-.1L8 14.5l-1.6-1.2-2 .1-.6-1.9-1.6-1.2.6-1.9-.6-1.9 1.6-1.2.6-1.9 2 .1zM5.6 8.2l1.6 1.5 3.2-3.2',
  scissors: 'M4.5 4.5a1.8 1.8 0 1 0 0 .1zM4.5 11.4a1.8 1.8 0 1 0 0 .1zM6 5.5l8 6M6 10.5l8-6',
  'minus.magnifyingglass': 'M7 2.5a4.5 4.5 0 1 0 0 9 4.5 4.5 0 0 0 0-9zM10.3 10.3 14 14M4.8 7h4.4',
  'plus.magnifyingglass': 'M7 2.5a4.5 4.5 0 1 0 0 9 4.5 4.5 0 0 0 0-9zM10.3 10.3 14 14M4.8 7h4.4M7 4.8v4.4',
  'arrow.up.left.and.down.right.magnifyingglass': 'M7 2.5a4.5 4.5 0 1 0 0 9 4.5 4.5 0 0 0 0-9zM10.3 10.3 14 14M5.2 5.2h2M5.2 5.2v2M8.8 8.8h-2M8.8 8.8v-2',
  magnifyingglass: 'M7 2.5a4.5 4.5 0 1 0 0 9 4.5 4.5 0 0 0 0-9zM10.3 10.3 14 14',
  'rectangle.grid.2x2': 'M2.2 2.2h5v5h-5zM8.8 2.2h5v5h-5zM2.2 8.8h5v5h-5zM8.8 8.8h5v5h-5z',
  'note.text.badge.plus': 'M2 3h8.5v10.5H2zM4 6h4.5M4 8.5h4.5M4 11h2.5M13 2v4M11 4h4',
  'rectangle.3.group': 'M1.8 3.5h12.4v9H1.8zM4 6h3v4H4zM9 6h3v4H9z',
  'rectangle.compress.vertical': 'M2 7h12M2 9h12M8 1.5v3.8M6.3 3.6 8 5.3l1.7-1.7M8 14.5v-3.8M6.3 12.4 8 10.7l1.7 1.7',
  grid: 'M2 2h12v12H2zM6 2v12M10 2v12M2 6h12M2 10h12',
  'play.fill': 'M4.5 2.5v11L13 8z',
  'stop.fill': 'M3.5 3.5h9v9h-9z',
  'lock.fill': 'M4 7.2h8v6.6H4zM5.5 7.2V5.3a2.5 2.5 0 0 1 5 0v1.9',
  'wand.and.stars': 'M2.5 13.5 10 6M9 4.5l2.5 2.5M12.5 1.8v2M11.5 2.8h2M4 2v2M3 3h2M13 9v2M12 10h2',
  sparkles: 'M6 2l1 3.2L10 6.2 7 7.4 6 10.5 5 7.4 2 6.2 5 5.2zM11.5 8.5l.6 1.8 1.8.6-1.8.6-.6 1.8-.6-1.8L9.1 10.9l1.8-.6z',
  'text.below.photo': 'M2.5 2h11v7.5h-11zM2.5 7.5l3-3 3 3 2-2 3 3M3 12h10M5 14.2h6',
  'chart.xyaxis.line': 'M2 2v12h12M4 11l3-4 2.5 2.5L13.5 4',
  'chart.bar.xaxis': 'M2 14h12M3.5 11v-4M7 11V4M10.5 11V7.5',
  'line.diagonal': 'M3 13 13 3',
  'arrow.up.left.and.arrow.down.right': 'M2.5 6.5v-4h4M2.5 2.5l4.5 4.5M13.5 9.5v4h-4M13.5 13.5 9 9',
  'arrow.down.right.and.arrow.up.left': 'M7 3v4H3M7 7 2.5 2.5M9 13V9h4M9 9l4.5 4.5',
  paperclip: 'M12.5 7.5 7.6 12.4a3 3 0 0 1-4.2-4.2l5.4-5.4a2 2 0 0 1 2.8 2.8L6.2 11a1 1 0 0 1-1.4-1.4L9.5 5',
  'arrow.counterclockwise': 'M3 8a5 5 0 1 0 1.6-3.7M2.6 2.2v2.8h2.8',
  eye: 'M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8zM8 6a2 2 0 1 0 0 4 2 2 0 0 0 0-4z',
  keyboard: 'M1.5 4h13v8h-13zM4 6.5h.01M6.3 6.5h.01M8.6 6.5h.01M10.9 6.5h.01M5 9.5h6',
  paintpalette: 'M8 1.8a6.2 6.2 0 0 0 0 12.4c1 0 1.4-.7 1-1.5-.5-1 .2-2 1.3-2h1.6a2.3 2.3 0 0 0 2.3-2.4A6.4 6.4 0 0 0 8 1.8zM4.8 7.2h.01M6.5 4.6h.01M9.6 4.6h.01M11.4 7h.01',
  'text.cursor': 'M5.5 2.5h1.5a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H5.5M10.5 2.5H9a1 1 0 0 0-1 1M10.5 13.5H9a1 1 0 0 1-1-1M6.5 8h3',
  key: 'M5.3 7.2a2.7 2.7 0 1 0 0 .1zM8 8h6M12 8v2.2M10.2 8v1.5',
  network: 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM1.8 8h12.4M8 1.8c-2.2 2.4-2.2 10 0 12.4M8 1.8c2.2 2.4 2.2 10 0 12.4',
  'shield.slash': 'M8 1.6 13 3.4v4.2c0 3.2-2.2 5.7-5 6.8-2.8-1.1-5-3.6-5-6.8V3.4zM2 2l12 12',
  'hand.raised': 'M5.2 8.6V3.4a.9.9 0 0 1 1.8 0v4.4M7 7.6V2.6a.9.9 0 0 1 1.8 0v5M8.8 7.4V3.2a.9.9 0 0 1 1.8 0v5.4M10.6 8.2V5.2a.9.9 0 0 1 1.8 0v4.6c0 2.6-1.8 4.4-4.2 4.4-1.8 0-2.8-.8-3.8-2.4L2.8 9.4a.9.9 0 0 1 1.5-1z',
  'point.3.connected.trianglepath.dotted': 'M4 3.5a1.5 1.5 0 1 0 0 .01M4 12.5a1.5 1.5 0 1 0 0 .01M12 8a1.5 1.5 0 1 0 0 .01M4 5v6M5.3 4.2 10.7 7.2M5.3 11.8l5.4-3',
  tag: 'M2.5 2.5h5.2l6 6-5.2 5.2-6-6zM5.2 5.2v.01',
  'network.slash': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM1.8 8h12.4M2 2l12 12',
  trash: 'M3 4.5h10M6 4.5V3h4v1.5M4.3 4.5l.6 9h6.2l.6-9M6.8 7v4.5M9.2 7v4.5',
  tray: 'M2 9.5 3.8 3h8.4L14 9.5v3.8H2zM2 9.5h3.5l.8 1.5h3.4l.8-1.5H14',
  'text.alignleft': 'M2 3.5h12M2 6.5h8M2 9.5h12M2 12.5h8',
  'folder.badge.plus': 'M1.8 3.5h4.4l1.4 1.6h6.6v7.9H1.8zM8 7.4v3.6M6.2 9.2h3.6',
  'arrow.clockwise': 'M13 8a5 5 0 1 1-1.6-3.7M13.4 2.2v2.8h-2.8',
  'checkmark': 'M3 8.5 6.4 12 13 4.5',
  'exclamationmark.circle': 'M8 1.8a6.2 6.2 0 1 0 0 12.4A6.2 6.2 0 0 0 8 1.8zM8 4.8v4M8 10.9v.2',
  'square.and.arrow.up': 'M5 6.5h-2v7.3h10V6.5h-2M8 9.5v-8M5.5 3.8 8 1.3l2.5 2.5',
  'rectangle.portrait.and.arrow.right': 'M9.5 3V2H2.5v12h7v-1M6.5 8h7.5M11.8 5.7 14 8l-2.2 2.3',
}

const P: Record<string, string> = { ...BASE, ...CANVAS, ...DETAIL, ...SHELL, ...CHAT }

/** Symbols drawn as a filled disc with the glyph knocked out. */
const FILLED: Record<string, string> = {
  'play.circle.fill': 'M6.5 5.2v5.6L10.9 8z',
  'stop.circle.fill': 'M5.8 5.8h4.4v4.4H5.8z',
  'checkmark.circle.fill': 'M5 8.2l2 2 4-4.2',
  'xmark.circle.fill': 'M5.6 5.6l4.8 4.8M10.4 5.6l-4.8 4.8',
  'pencil.circle.fill': 'M5.4 10.6l.4-1.8 3.8-3.8 1.4 1.4-3.8 3.8z',
  'exclamationmark.triangle.fill': '',
  'xmark.octagon.fill': '',
  'circle.fill': '',
  'pause.circle.fill': 'M6.4 5.4v5.2M9.6 5.4v5.2',
  'bolt.circle.fill': 'M8.8 4.2 5.6 8.6h2.8l-1.2 3.2 3.2-4.4H7.6z',
  'arrow.triangle.2.circlepath.circle.fill': 'M11 7a3.2 3.2 0 0 0-5.6-1.2M5 9a3.2 3.2 0 0 0 5.6 1.2M5.2 4.4v1.6h1.6M10.8 11.6V10H9.2',
}

export function Icon({ name, size = 14, style, className, title }: { name: string; size?: number; style?: CSSProperties; className?: string; title?: string }) {
  const common = { width: size, height: size, viewBox: '0 0 16 16', className: 'icon ' + (className ?? ''), style, 'aria-hidden': !title, role: title ? 'img' : undefined }
  if (name === 'exclamationmark.triangle.fill') {
    return (
      <svg {...common}>
        {title && <title>{title}</title>}
        <path d="M8 1.6 15 14H1z" fill="currentColor" />
        <path d="M8 6.2v3.6M8 11.6v.2" stroke="var(--glyph-on-fill)" strokeWidth={1.6} strokeLinecap="round" fill="none" />
      </svg>
    )
  }
  if (name === 'xmark.octagon.fill') {
    return (
      <svg {...common}>
        {title && <title>{title}</title>}
        <path d="M5.3 1.5h5.4l3.8 3.8v5.4l-3.8 3.8H5.3l-3.8-3.8V5.3z" fill="currentColor" />
        <path d="M6 6l4 4M10 6l-4 4" stroke="var(--glyph-on-fill)" strokeWidth={1.6} strokeLinecap="round" fill="none" />
      </svg>
    )
  }
  if (name in FILLED) {
    return (
      <svg {...common}>
        {title && <title>{title}</title>}
        <circle cx="8" cy="8" r="6.6" fill="currentColor" />
        {FILLED[name] && <path d={FILLED[name]} stroke="var(--glyph-on-fill)" strokeWidth={1.6} strokeLinecap="round" strokeLinejoin="round"
          fill={name.startsWith('play') || name.startsWith('stop') ? 'var(--glyph-on-fill)' : 'none'} />}
      </svg>
    )
  }
  const d = P[name] ?? P.circle
  const solid = name === 'play.fill' || name === 'stop.fill' || name === 'lock.fill' || name === 'square.stack.3d.up.fill' || name === 'bubble.left.and.bubble.right.fill' ||
    name === 'checkmark.square.fill'
  return (
    <svg {...common}>
      {title && <title>{title}</title>}
      <path d={d} fill={solid ? 'currentColor' : 'none'} fillOpacity={name === 'square.stack.3d.up.fill' || name === 'bubble.left.and.bubble.right.fill' || name === 'checkmark.square.fill' ? 0.35 : 1}
        stroke="currentColor" strokeWidth={1.35} strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  )
}

/** Semantic colors used by state labels (never color alone: icons and text too). */
export const tint = (c: string) => `var(--${c})`
