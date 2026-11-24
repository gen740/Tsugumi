export const sampleRate = 48_000
export const viewFlags = { hideCursor: 1, lockPointer: 2 } as const

export type ViewSize = { width: number; height: number; scale: number }
export type TextArea = { x: number; y: number; width: number; height: number }
export type ViewInput =
  | { type: 'pointer'; kind: number; x: number; y: number; buttons: number; wheelX: number; wheelY: number; modifiers: number }
  | { type: 'key'; kind: number; code: number; modifiers: number }
  | { type: 'text'; text: string }
  | { type: 'gamepad'; pad: number; kind: number; buttons: number; lx: number; ly: number; rx: number; ry: number; lt: number; rt: number }
export type DawnInput = ViewInput | { type: 'composition'; text: string; cursor: number }
export type ViewFeatures = { pointer: boolean; key: boolean; gamepad: boolean; composition: boolean; audio: boolean }
export type AudioClock = { contextTime: number; absoluteTime: number }
export type AudioRequest = { frame: number; frames: number }
export type AudioChunk = { frame: number; samples?: Float32Array<ArrayBuffer> }

export type ViewCommand =
  | { type: 'start'; bytes: Uint8Array<ArrayBuffer>; canvas: OffscreenCanvas; size: ViewSize; routes: string[]; refreshPeriod: number; visible: boolean }
  | { type: 'route'; route: string }
  | { type: 'resize'; size: ViewSize; at: number; id: number }
  | { type: 'refresh'; period: number }
  | { type: 'visible'; visible: boolean }
  | { type: 'message'; bytes: Uint8Array<ArrayBuffer>; at: number; id: number }
  | { type: 'input'; event: DawnInput; at: number; id: number }
  | { type: 'snapshot'; id: number }
  | { type: 'sendResult'; id: number; error?: string }
  | { type: 'audio'; port: MessagePort; clock: AudioClock }
  | { type: 'audioClock'; clock: AudioClock }

export type ViewNotice =
  | { type: 'ready'; features: ViewFeatures }
  | { type: 'call'; started: boolean }
  | { type: 'complete'; id: number; error?: string; png?: Uint8Array<ArrayBuffer> }
  | { type: 'send'; id: number; bytes: Uint8Array<ArrayBuffer>; route: Uint8Array<ArrayBuffer> }
  | { type: 'fatal' | 'gpuError' | 'sendError'; message: string }
  | { type: 'textArea'; area: TextArea | null }

export const routeKey = (route: Uint8Array) =>
  Array.from(route, byte => byte.toString(16).padStart(2, '0')).join('')
