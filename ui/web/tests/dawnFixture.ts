import { viewExport, type ViewCallback } from '../src/dawn/loadView'

export const i32 = 0x7f
export const i64 = 0x7e
export const f32 = 0x7d

interface FunctionBody { params?: number[]; code: number[] }
interface Import { module: string; name: string; memory?: boolean; params?: number[]; results?: number[] }

export function uleb(value: number): number[] {
  const bytes: number[] = []
  do {
    const low = value & 0x7f
    value >>>= 7
    bytes.push(low | (value ? 0x80 : 0))
  } while (value)
  return bytes
}

const vec = (elements: number[][]) => [...uleb(elements.length), ...elements.flat()]
const str = (value: string) => {
  const bytes = new TextEncoder().encode(value)
  return [...uleb(bytes.length), ...bytes]
}
const section = (id: number, bytes: number[]) => [id, ...uleb(bytes.length), ...bytes]

export function encodeView({
  imports = [],
  missing,
  pages = 1,
  callbacks = {},
}: {
  imports?: Import[]
  missing?: ViewCallback | 'memory'
  pages?: number
  callbacks?: Partial<Record<ViewCallback, FunctionBody>>
} = {}): Uint8Array<ArrayBuffer> {
  const definitions: Partial<Record<ViewCallback, FunctionBody>> = {
    abi: { code: [0x41, 0] },
    start: { params: [i32, i32, i32, i32, i32, f32], code: [0x41, 0] },
    resize: { params: [i32, i32, f32, i64], code: [0x41, 0] },
    draw: { params: [i32, i64], code: [0x41, 1] },
    ...callbacks,
  }
  const bodies = Object.entries(definitions) as [ViewCallback, FunctionBody][]
  const signatures = [...imports.filter(input => !input.memory), ...bodies.map(([, body]) => body)]
  const functionImports = imports.filter(input => !input.memory).length
  let nextImport = 0
  const exports = bodies.flatMap(([name], index) =>
    name === missing ? [] : [[...str(viewExport(name)), 0, ...uleb(functionImports + index)]])
  if (missing !== 'memory') exports.push([...str('memory'), 2, 0])
  const hasMemoryImport = imports.some(input => input.memory)

  return new Uint8Array([
    0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
    ...section(1, vec(signatures.map(signature => {
      const { params = [] } = signature
      const results = ('results' in signature ? signature.results : undefined) ?? [i32]
      return [0x60, ...uleb(params.length), ...params, ...uleb(results.length), ...results]
    }))),
    ...section(2, vec(imports.map(({ module, name, memory }) => [
      ...str(module), ...str(name), ...(memory ? [2, 0, 1] : [0, ...uleb(nextImport++)]),
    ]))),
    ...section(3, vec(bodies.map((_, index) => uleb(functionImports + index)))),
    ...section(5, vec(hasMemoryImport ? [] : [[0, ...uleb(pages)]])),
    ...section(7, vec(exports)),
    ...section(10, vec(bodies.map(([, { code }]) => [...uleb(code.length + 2), 0, ...code, 0x0b]))),
  ])
}
