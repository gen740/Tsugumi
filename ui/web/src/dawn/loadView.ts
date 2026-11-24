import { importNames } from '../../shared/gen/webgpu_imports.gen.js'

const webgpuNames = new Set<string>(importNames)
const tsugumiNames = new Set(['tsugumi_view_send', 'tsugumi_view_read'])

export type ViewCallback = 'abi' | 'start' | 'resize' | 'draw' | 'message' | 'pointer' | 'key'
  | 'audio' | 'text' | 'text_input' | 'composition' | 'gamepad'
export const viewExport = (name: ViewCallback) => `tsugumi_view_${name}`

export function validateViewModule(module: WebAssembly.Module): void {
  for (const { module: namespace, name, kind } of WebAssembly.Module.imports(module)) {
    const names = namespace === 'webgpu' ? webgpuNames : namespace === 'tsugumi' ? tsugumiNames : undefined
    if (kind !== 'function' || !names?.has(name))
      throw new Error(`Unsupported Dawn view import: ${namespace}.${name} (${kind})`)
  }
  const exports = new Map(WebAssembly.Module.exports(module).map(({ name, kind }) => [name, kind]))
  if (exports.get('memory') !== 'memory') throw new Error('Dawn view must export memory')
  for (const callback of ['abi', 'start', 'resize', 'draw'] as const) {
    const name = viewExport(callback)
    if (exports.get(name) !== 'function') throw new Error(`Dawn view must export ${name} as a function`)
  }
}

export async function loadView(bytes: Uint8Array<ArrayBuffer>): Promise<{ module: WebAssembly.Module }> {
  const module = await WebAssembly.compile(bytes)
  validateViewModule(module)
  return { module }
}

export function checkViewAbi(value: unknown): void {
  if (value !== 0) throw new Error(`Unsupported Dawn view ABI: ${String(value)} (expected 0)`)
}
