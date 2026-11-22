import { Builder, ByteBuffer } from 'flatbuffers'
import { CardPathT } from './gen/tsugumi/data/card-path'
import { ClientView, ClientViewT } from './gen/tsugumi/rpc/client-view'
import { ViewCameraT } from './gen/tsugumi/rpc/view-camera'

// The project core checks the buffer. The clients own presentation semantics.
export interface ViewCamera { scope: string[]; x: number; y: number; zoom: number }
export interface ClientViewState { scope: string[]; cameras: ViewCamera[]; selected_cards: string[][] }
const valid = (c: ViewCamera) => Number.isFinite(c.x) && Number.isFinite(c.y) && Number.isFinite(c.zoom) && c.zoom > 0

export function readViewState(bytes: number[]): ClientViewState | undefined {
  if (!bytes.length) return undefined
  try {
    const view = ClientView.getRootAsClientView(new ByteBuffer(Uint8Array.from(bytes))).unpack()
    return {
      scope: view.scope.map(String),
      cameras: view.cameras.map(c => ({ ...c, scope: c.scope.map(String) })).filter(valid),
      selected_cards: view.selectedCards.map(c => c.segments.map(String)),
    }
  } catch { return undefined }
}

export function writeViewState(view: ClientViewState): number[] {
  const builder = new Builder()
  const root = new ClientViewT(view.scope,
    view.cameras.filter(valid).map(c => new ViewCameraT(c.scope, c.x, c.y, c.zoom)),
    view.selected_cards.map(path => new CardPathT(path)))
  builder.finish(root.pack(builder))
  return [...builder.asUint8Array()]
}
