// Renders recorded FlatBuffers values as JSON using a reflection schema.
// Unions use the flatc JSON convention (`<field>_type` + `<field>`).
// 64 bit integers are decimal strings in JSON beyond 2^53.
import { ByteBuffer } from 'flatbuffers'
import { BaseType, BFBS, Field } from '../../shared/bfbs'
import { L } from '../i18n'

type Json = unknown
type JsonObject = Record<string, Json>

const MAX_SAFE = 9007199254740991n
const isObject = (v: Json): v is JsonObject => typeof v === 'object' && v !== null && !Array.isArray(v)

export class FlatJSON {
  constructor(readonly schema: BFBS) {}

  // ---- FlatBuffer -> JSON ------------------------------------------------------

  /** Renders a table as JSON. Binary schemas are summarized, and byte vectors
   *  become text when they are valid UTF-8. */
  json(object: number, bb: ByteBuffer, t: number, depth = 0): JsonObject {
    const out: JsonObject = {}
    const obj = this.schema.objects[object]
    if (depth > 24) return { truncated: true }
    for (const f of obj.fields) {
      const o = bb.__offset(t, f.offset)
      if (!o) continue
      const p = t + o
      switch (f.type.base) {
        case BaseType.bool: out[f.name] = !!bb.readInt8(p); break
        case BaseType.byte: out[f.name] = this.enumName(f, bb.readInt8(p)); break
        case BaseType.ubyte: out[f.name] = this.enumName(f, bb.readUint8(p)); break
        case BaseType.short: out[f.name] = this.enumName(f, bb.readInt16(p)); break
        case BaseType.ushort: out[f.name] = this.enumName(f, bb.readUint16(p)); break
        case BaseType.int: out[f.name] = bb.readInt32(p); break
        case BaseType.uint: out[f.name] = bb.readUint32(p); break
        case BaseType.long: out[f.name] = big(bb.readInt64(p)); break
        case BaseType.ulong: out[f.name] = big(bb.readUint64(p)); break
        case BaseType.float: out[f.name] = bb.readFloat32(p); break
        case BaseType.double: out[f.name] = bb.readFloat64(p); break
        case BaseType.string: out[f.name] = bb.__string(p); break
        case BaseType.obj: out[f.name] = this.json(f.type.index, bb, bb.__indirect(p), depth + 1); break
        case BaseType.utype: {
          const raw = bb.readUint8(p)
          out[f.name] = this.schema.enums[f.type.index].values.find((v) => v.value === raw)?.name ?? raw
          break
        }
        case BaseType.union: {
          const tf = obj.fields.find((x) => x.name === `${f.name}_type`)
          if (!tf) break
          const to = bb.__offset(t, tf.offset)
          const raw = to ? bb.readUint8(t + to) : 0
          const ev = this.schema.enums[f.type.index].values.find((v) => v.value === raw)
          if (!ev?.unionType) break
          out[f.name] = this.json(ev.unionType.index, bb, bb.__indirect(p), depth + 1)
          break
        }
        case BaseType.vector: out[f.name] = this.vectorJSON(f, bb, p, depth); break
      }
    }
    return out
  }

  private vectorJSON(f: Field, bb: ByteBuffer, p: number, depth: number): Json {
    const n = bb.__vector_len(p)
    const start = bb.__vector(p)
    switch (f.type.element) {
      case BaseType.ubyte:
      case BaseType.byte: {
        if (f.name.includes('bfbs')) return { binary_schema_bytes: n }
        const bytes = bb.bytes().subarray(start, start + n)
        try {
          return new TextDecoder('utf-8', { fatal: true }).decode(bytes)
        } catch {
          return { base64: toBase64(bytes) }
        }
      }
      case BaseType.string:
        return Array.from({ length: n }, (_, i) => bb.__string(start + i * 4))
      case BaseType.obj:
        return Array.from({ length: n }, (_, i) => this.json(f.type.index, bb, bb.__indirect(start + i * 4), depth + 1))
      case BaseType.double: return Array.from({ length: n }, (_, i) => bb.readFloat64(start + i * 8))
      case BaseType.float: return Array.from({ length: n }, (_, i) => bb.readFloat32(start + i * 4))
      case BaseType.ulong: return Array.from({ length: n }, (_, i) => big(bb.readUint64(start + i * 8)))
      case BaseType.long: return Array.from({ length: n }, (_, i) => big(bb.readInt64(start + i * 8)))
      case BaseType.int: return Array.from({ length: n }, (_, i) => bb.readInt32(start + i * 4))
      case BaseType.uint: return Array.from({ length: n }, (_, i) => bb.readUint32(start + i * 4))
      case BaseType.short: return Array.from({ length: n }, (_, i) => bb.readInt16(start + i * 2))
      case BaseType.ushort: return Array.from({ length: n }, (_, i) => bb.readUint16(start + i * 2))
      case BaseType.bool: return Array.from({ length: n }, (_, i) => !!bb.readInt8(start + i))
      default: return { elements: n }
    }
  }

  private enumName(f: Field, raw: number): Json {
    if (f.type.index < 0) return raw
    return this.schema.enums[f.type.index].values.find((v) => v.value === raw)?.name ?? raw
  }
}

const big = (v: bigint): Json => (v > MAX_SAFE || v < -MAX_SAFE ? v.toString() : Number(v))

export function toBase64(bytes: Uint8Array): string {
  let s = ''
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000))
  return btoa(s)
}

/** A recorded value (inline FlatBuffer with its own BFBS) as pretty JSON. */
export function describeValue(v: {
  name: string
  rootType: string
  sizeBytes: bigint | null
  artifacts?: { id: string; sizeBytes: bigint | null }[]
  inlineBytes: number[]
  schemaBfbs: number[]
}): string {
  let head = `${v.name} : ${v.rootType}`
  if (v.sizeBytes !== null) head += ` · ${v.sizeBytes} bytes`
  for (const artifact of v.artifacts ?? []) {
    head += '\n' + L('成果物 {0}', artifact.id)
    if (artifact.sizeBytes !== null) head += ` · ${artifact.sizeBytes} bytes`
  }
  if (!v.inlineBytes.length) return head + L('\n（内容は記録されていません）')
  const hex = () => v.inlineBytes.slice(0, 256).map((x) => x.toString(16).padStart(2, '0')).join(' ')
  try {
    const schema = new BFBS(new Uint8Array(v.schemaBfbs))
    const index = schema.find(v.rootType)
    if (index < 0) return head + '\n' + hex()
    const bb = new ByteBuffer(new Uint8Array(v.inlineBytes))
    const obj = new FlatJSON(schema).json(index, bb, bb.readInt32(0))
    return head + '\n' + JSON.stringify(sortKeys(obj), null, 2)
  } catch {
    return head + '\n' + hex()
  }
}

function sortKeys(v: Json): Json {
  if (Array.isArray(v)) return v.map(sortKeys)
  if (isObject(v)) return Object.fromEntries(Object.keys(v).sort().map((k) => [k, sortKeys(v[k])]))
  return v
}
