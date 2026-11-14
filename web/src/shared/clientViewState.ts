// Client-owned presentation state. The project core only stores its JSON.
export interface ViewCamera { scope: string[]; x: number; y: number; zoom: number }
export interface ClientViewState { scope: string[]; cameras: ViewCamera[]; selected_cards: string[][] }
const path = (p: unknown): p is string[] => Array.isArray(p) && p.every(x => typeof x === 'string')
const camera = (c: any): c is ViewCamera => c && path(c.scope) && Number.isFinite(c.x) && Number.isFinite(c.y) && Number.isFinite(c.zoom) && c.zoom > 0

export function readViewState(json: string): ClientViewState | undefined {
  try {
    const v = JSON.parse(json)
    if (!v || !path(v.scope)) return undefined
    return { scope: v.scope, cameras: Array.isArray(v.cameras) ? v.cameras.filter(camera) : [],
      selected_cards: Array.isArray(v.selected_cards) ? v.selected_cards.filter(path) : [] }
  } catch { return undefined }
}
export function writeViewState(view: ClientViewState): string {
  return JSON.stringify({ ...view, cameras: view.cameras.filter(camera) })
}
