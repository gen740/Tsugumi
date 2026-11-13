// What a canvas draws in one scope, as the core is asked to arrange or untangle
// it (LayoutArrange / LayoutUntangle of protocol/tsugumi.fbs), and what it
// proposes. The layout itself is worked out in tsugumid (core/src/layout.cc),
// once for every client; here are only the measurements and the answer
// (`RPCClient.layoutArrange` and `layoutUntangle` send them).
import type { FlowDirection } from './prefs'

export interface LayoutPort { name: string; x: number; y: number }
export interface LayoutCard {
  id: string
  x: number
  y: number
  width: number
  height: number
  /** Its ports may be drawn in another order. */
  reorderable: boolean
  /** Where each port is on the Card (from `CardGeometry`), in the order drawn. */
  inputs: LayoutPort[]
  outputs: LayoutPort[]
}
export interface LayoutWire { from: string; fromPort: string; to: string; toPort: string }
/** An output frame (`id` is its Card's) or a note. */
export interface LayoutBox { id: string; x: number; y: number; width: number; height: number }
export interface LayoutBarEnd { name: string; card: string; port: string }
/** A bar of a Card in Card; `at` only when it was put somewhere by hand. */
export interface LayoutBar { ends: LayoutBarEnd[]; at?: { x: number; y: number } }
export interface LayoutScene {
  flow: FlowDirection
  grid: number
  snap: boolean
  cards: LayoutCard[]
  wires: LayoutWire[]
  frames: LayoutBox[]
  notes: LayoutBox[]
  inputs?: LayoutBar
  outputs?: LayoutBar
}

export interface LayoutPlace { id: string; x: number; y: number }
export interface LayoutPortOrder { id: string; inputs: string[]; outputs: string[] }
export interface LayoutProposal { cards: LayoutPlace[]; frames: LayoutPlace[]; notes: LayoutPlace[]; portOrders: LayoutPortOrder[] }
