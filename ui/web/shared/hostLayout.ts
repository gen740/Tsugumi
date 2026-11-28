import * as H from './gen/tsugumi/host'
import type { LayoutBar, LayoutBox, LayoutPort, LayoutScene } from './layoutScene'
export { H }
export function sceneRequest(scene: LayoutScene): H.LayoutSceneT {
  const ports = (list: LayoutPort[]) => list.map((p) => new H.LayoutPortT(p.name, p.x, p.y))
  const boxes = (list: LayoutBox[]) => list.map((b) => new H.LayoutBoxT(b.id, b.x, b.y, b.width, b.height))
  const bar = (b: LayoutBar | undefined) => b && new H.LayoutBarT(b.ends.map((e) => new H.LayoutBarEndT(e.name, e.card, e.port)), b.at?.x ?? null, b.at?.y ?? null)
  return new H.LayoutSceneT(
    scene.grid, scene.snap,
    scene.cards.map((c) => new H.LayoutCardT(c.id, c.x, c.y, c.width, c.height, ports(c.inputs), ports(c.outputs), c.reorderable)),
    scene.wires.map((w) => new H.LayoutWireT(w.from, w.fromPort, w.to, w.toPort)),
    boxes(scene.notes), bar(scene.inputs) ?? null, bar(scene.outputs) ?? null,
  )
}

