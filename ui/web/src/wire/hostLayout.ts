import * as H from '../../shared/gen/tsugumi/host'
import type { VersionCanvasT } from '../../shared/gen/tsugumi/rpc/version-canvas'
import { hostCall } from './host'
import { s } from './rpc'
export { H, sceneRequest } from '../../shared/hostLayout'

export const layoutHost = {
  complete: async (project: string, layout: string, definitions: Record<string, string>) =>
    s((await hostCall<H.LayoutValidatedT>(H.RequestPayload.LayoutCompleteRequest,
      new H.LayoutCompleteRequestT(project, layout, JSON.stringify(definitions)), H.ReplyPayload.LayoutValidated)).json),
  versionCanvas: (data: VersionCanvasT) => hostCall<H.VersionCanvasT>(H.RequestPayload.VersionCanvasRequest,
    new H.VersionCanvasRequestT(data.layout, data.cards.map(c => new H.VersionCanvasCardT(c.id, 0, 0, c.kind, c.fan)),
      data.wires.map(w => new H.VersionCanvasWireT(w.from, w.to))), H.ReplyPayload.VersionCanvas),
}
