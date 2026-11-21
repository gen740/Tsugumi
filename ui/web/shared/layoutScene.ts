// What a canvas draws in one scope, as the core is asked to arrange or untangle
// it (LayoutArrange / LayoutUntangle of protocol/host.fbs), and what it
// proposes. The layout itself is worked out in tsugumi-hostd (hostd/src/layout.cc),
// once for every client; here are only the measurements and the answer
// (`layoutHost.layoutArrange` and `layoutUntangle` send them).
export type FlowDirection = 'topToBottom' | 'leftToRight'

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
/** A note. */
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
  notes: LayoutBox[]
  inputs?: LayoutBar
  outputs?: LayoutBar
}
