import * as H from '../../shared/gen/tsugumi/host'
import { hostCall } from './host'

import type { ViewInput } from '../dawn/workerProtocol'

function encode(input: ViewInput): [H.ViewAction, H.ViewActionRequestT['action']] {
  switch (input.type) {
    case 'pointer': return [H.ViewAction.ViewPointer, new H.ViewPointerT(input.kind, input.x, input.y, input.buttons, input.wheelX, input.wheelY, input.modifiers)]
    case 'key': return [H.ViewAction.ViewKey, new H.ViewKeyT(input.kind, input.code, input.modifiers)]
    case 'text': return [H.ViewAction.ViewText, new H.ViewTextT(input.text)]
    case 'gamepad': return [H.ViewAction.ViewGamepad, new H.ViewGamepadT(input.pad, input.kind, input.buttons, input.lx, input.ly, input.rx, input.ry, input.lt, input.rt)]
  }
}

export function decodeViewInput(action: H.ViewActionDispatchT['action']): ViewInput | undefined {
  if (action instanceof H.ViewPointerT) {
    const { kind, x, y, buttons, wheelX, wheelY, modifiers } = action
    return { type: 'pointer', kind, x, y, buttons, wheelX, wheelY, modifiers }
  }
  if (action instanceof H.ViewKeyT) {
    const { kind, code, modifiers } = action
    return { type: 'key', kind, code, modifiers }
  }
  if (action instanceof H.ViewTextT) {
    return { type: 'text', text: typeof action.text === 'string' ? action.text : new TextDecoder().decode(action.text ?? undefined) }
  }
  if (action instanceof H.ViewGamepadT) {
    const { pad, kind, buttons, lx, ly, rx, ry, lt, rt } = action
    return { type: 'gamepad', pad, kind, buttons, lx, ly, rx, ry, lt, rt }
  }
  return undefined
}

export const hostViews = {
  async list(project: string): Promise<H.ViewWindowT[]> {
    const reply = await hostCall<H.ViewWindowsT>(H.RequestPayload.ViewWindowsRequest,
      new H.ViewWindowsRequestT(project), H.ReplyPayload.ViewWindows)
    return reply.windows
  },
  async sendInput(project: string, session: string, input: ViewInput): Promise<void> {
    const [type, value] = encode(input)
    await hostCall<H.ViewActionResultT>(H.RequestPayload.ViewActionRequest,
      new H.ViewActionRequestT(project, session, type, value), H.ReplyPayload.ViewActionResult)
  },
  async snapshot(project: string, session: string): Promise<Uint8Array<ArrayBuffer>> {
    const result = await hostCall<H.ViewActionResultT>(H.RequestPayload.ViewActionRequest,
      new H.ViewActionRequestT(project, session, H.ViewAction.ViewSnapshot, new H.ViewSnapshotT()),
      H.ReplyPayload.ViewActionResult)
    return Uint8Array.from(result.png)
  },
}
