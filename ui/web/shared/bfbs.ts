// Minimal reader for FlatBuffers binary schemas (reflection.fbs layout).
// Tool schemas, tool argument/result conversion and the display of recorded
// values are derived from BFBS so nothing hand-maintains the RPC shape.
import { ByteBuffer } from 'flatbuffers'

export enum BaseType {
  none = 0, utype, bool, byte, ubyte, short, ushort, int, uint, long, ulong, float, double,
  string, vector, obj, union, array, vector64,
}

export interface TypeInfo {
  base: BaseType
  element: BaseType
  index: number
}

export interface Field {
  name: string
  type: TypeInfo
  id: number
  offset: number // vtable offset (4 + 2 * id)
  required: boolean
  optional: boolean
  documentation: string[]
}

export interface BObject {
  name: string
  fields: Field[]
  isStruct: boolean
  documentation: string[]
}

export interface EnumValue {
  name: string
  value: number
  unionType?: TypeInfo
}

export interface BEnum {
  name: string
  values: EnumValue[]
  isUnion: boolean
}

const vo = (id: number) => 4 + 2 * id

function tables(bb: ByteBuffer, t: number, id: number): number[] {
  const o = bb.__offset(t, vo(id))
  if (!o) return []
  const n = bb.__vector_len(t + o)
  const start = bb.__vector(t + o)
  const out: number[] = []
  for (let i = 0; i < n; i++) out.push(bb.__indirect(start + i * 4))
  return out
}

function string(bb: ByteBuffer, t: number, id: number): string {
  const o = bb.__offset(t, vo(id))
  return o ? (bb.__string(t + o) as string) : ''
}

function strings(bb: ByteBuffer, t: number, id: number): string[] {
  const o = bb.__offset(t, vo(id))
  if (!o) return []
  const n = bb.__vector_len(t + o)
  const start = bb.__vector(t + o)
  const out: string[] = []
  for (let i = 0; i < n; i++) out.push(bb.__string(start + i * 4) as string)
  return out
}

function child(bb: ByteBuffer, t: number, id: number): number | undefined {
  const o = bb.__offset(t, vo(id))
  return o ? bb.__indirect(t + o) : undefined
}

function i8(bb: ByteBuffer, t: number, id: number, d: number) {
  const o = bb.__offset(t, vo(id))
  return o ? bb.readInt8(t + o) : d
}
function bool(bb: ByteBuffer, t: number, id: number) {
  const o = bb.__offset(t, vo(id))
  return o ? !!bb.readInt8(t + o) : false
}
function u16(bb: ByteBuffer, t: number, id: number) {
  const o = bb.__offset(t, vo(id))
  return o ? bb.readUint16(t + o) : 0
}
function i32(bb: ByteBuffer, t: number, id: number, d: number) {
  const o = bb.__offset(t, vo(id))
  return o ? bb.readInt32(t + o) : d
}
function i64(bb: ByteBuffer, t: number, id: number) {
  const o = bb.__offset(t, vo(id))
  return o ? Number(bb.readInt64(t + o)) : 0
}

function typeInfo(bb: ByteBuffer, t: number | undefined): TypeInfo {
  if (t === undefined) return { base: BaseType.none, element: BaseType.none, index: -1 }
  return { base: i8(bb, t, 0, 0), element: i8(bb, t, 1, 0), index: i32(bb, t, 2, -1) }
}

export class BFBS {
  readonly objects: BObject[]
  readonly enums: BEnum[]

  constructor(data: Uint8Array) {
    if (data.length < 8) throw new Error('schema too short')
    const bb = new ByteBuffer(data)
    const root = bb.readInt32(0)
    this.objects = tables(bb, root, 0).map((t) => ({
      name: string(bb, t, 0),
      fields: tables(bb, t, 1)
        .map((f) => ({
          name: string(bb, f, 0),
          type: typeInfo(bb, child(bb, f, 1)),
          id: u16(bb, f, 2),
          offset: u16(bb, f, 3),
          required: bool(bb, f, 7),
          optional: bool(bb, f, 11),
          documentation: strings(bb, f, 10),
        }))
        .sort((a, b) => a.id - b.id),
      isStruct: bool(bb, t, 2),
      documentation: strings(bb, t, 6),
    }))
    this.enums = tables(bb, root, 1).map((t) => ({
      name: string(bb, t, 0),
      values: tables(bb, t, 1).map((v) => {
        const ut = child(bb, v, 3)
        return { name: string(bb, v, 0), value: i64(bb, v, 1), unionType: ut === undefined ? undefined : typeInfo(bb, ut) }
      }),
      isUnion: bool(bb, t, 2),
    }))
  }

  object(name: string): number {
    return this.objects.findIndex((o) => o.name === name)
  }

  /** By full name, or by the last component when the full name is unknown. */
  find(rootType: string): number {
    const i = this.object(rootType)
    return i >= 0 ? i : this.objects.findIndex((o) => o.name.endsWith('.' + rootType))
  }
}
