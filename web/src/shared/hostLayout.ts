// Stateless presentation operations implemented by tsugumi-agentd.
import * as H from '../gen/tsugumi/host'
import type { LayoutBar, LayoutBox, LayoutPort, LayoutProposal, LayoutScene } from '../model/layoutScene'
import { api } from '../api'
export { H }
export const layoutHost = {
  layoutArrange: (scene: LayoutScene) => api<LayoutProposal>('/api/layout/arrange', { scene }),
  layoutUntangle: (scene: LayoutScene, around: string[]) => api<LayoutProposal>('/api/layout/untangle', { scene, around }),
  validate: async (json: string, removedCards: string[] = []) => (await api<{ json: string }>('/api/layout/validate', { json, removedCards })).json,
  versionCanvas: (data: unknown) => api<H.VersionCanvasT>('/api/layout/version-canvas', data),
}
export function sceneRequest(scene: LayoutScene): H.LayoutSceneT {
  const ports = (list: LayoutPort[]) => list.map((p) => new H.LayoutPortT(p.name, p.x, p.y))
  const boxes = (list: LayoutBox[]) => list.map((b) => new H.LayoutBoxT(b.id, b.x, b.y, b.width, b.height))
  const bar = (b: LayoutBar | undefined) => b && new H.LayoutBarT(b.ends.map((e) => new H.LayoutBarEndT(e.name, e.card, e.port)), b.at?.x ?? null, b.at?.y ?? null)
  return new H.LayoutSceneT(
    scene.flow, scene.grid, scene.snap,
    scene.cards.map((c) => new H.LayoutCardT(c.id, c.x, c.y, c.width, c.height, ports(c.inputs), ports(c.outputs), c.reorderable)),
    scene.wires.map((w) => new H.LayoutWireT(w.from, w.fromPort, w.to, w.toPort)),
    boxes(scene.frames), boxes(scene.notes), bar(scene.inputs) ?? null, bar(scene.outputs) ?? null,
  )
}

