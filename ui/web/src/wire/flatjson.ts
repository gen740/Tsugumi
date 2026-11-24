// Renders recorded FlatBuffers values as JSON using a reflection schema.
// Unions use the flatc JSON convention (`<field>_type` + `<field>`).
// 64 bit integers are decimal strings in JSON beyond 2^53.
import { ByteBuffer } from 'flatbuffers'
import { BaseType, BFBS, Field } from '../../shared/bfbs'
import { L } from '../i18n'
import { D, s } from './rpc'

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
        case BaseType.obj: out[f.name] = this.schema.objects[f.type.index].isStruct ? this.structJSON(f.type.index, bb, p, depth + 1) : this.json(f.type.index, bb, bb.__indirect(p), depth + 1); break
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
      case BaseType.obj: {
        const element = this.schema.objects[f.type.index]
        if (element.isStruct) return Array.from({ length: n }, (_, i) => this.structJSON(f.type.index, bb, start + i * element.byteSize, depth + 1))
        return Array.from({ length: n }, (_, i) => this.json(f.type.index, bb, bb.__indirect(start + i * 4), depth + 1))
      }
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

  /** A struct stored inline at `p`. A tsugumi.ArtifactId reads as its uuid text. */
  private structJSON(object: number, bb: ByteBuffer, p: number, depth: number): Json {
    const obj = this.schema.objects[object]
    if (obj.name === ARTIFACT_ID && obj.byteSize === 16) return uuidText(bb.bytes().subarray(p, p + 16))
    if (depth > 24) return { truncated: true }
    const out: JsonObject = {}
    for (const f of obj.fields) {
      const at = p + f.offset
      if (f.type.base === BaseType.array) {
        const element = f.type.element === BaseType.obj ? this.schema.objects[f.type.index] : undefined
        const size = element ? element.byteSize : SCALAR_SIZE[f.type.element]
        if (!size) continue
        out[f.name] = Array.from({ length: f.type.fixedLength }, (_, i) =>
          element ? this.structJSON(f.type.index, bb, at + i * size, depth + 1) : this.scalar(f, f.type.element, bb, at + i * size))
      } else if (f.type.base === BaseType.obj) {
        out[f.name] = this.structJSON(f.type.index, bb, at, depth + 1)
      } else {
        out[f.name] = this.scalar(f, f.type.base, bb, at)
      }
    }
    return out
  }

  private scalar(f: Field, base: BaseType, bb: ByteBuffer, p: number): Json {
    switch (base) {
      case BaseType.bool: return !!bb.readInt8(p)
      case BaseType.byte: return this.enumName(f, bb.readInt8(p))
      case BaseType.ubyte: return this.enumName(f, bb.readUint8(p))
      case BaseType.short: return this.enumName(f, bb.readInt16(p))
      case BaseType.ushort: return this.enumName(f, bb.readUint16(p))
      case BaseType.int: return bb.readInt32(p)
      case BaseType.uint: return bb.readUint32(p)
      case BaseType.long: return big(bb.readInt64(p))
      case BaseType.ulong: return big(bb.readUint64(p))
      case BaseType.float: return bb.readFloat32(p)
      case BaseType.double: return bb.readFloat64(p)
      default: return null
    }
  }

  private enumName(f: Field, raw: number): Json {
    if (f.type.index < 0) return raw
    return this.schema.enums[f.type.index].values.find((v) => v.value === raw)?.name ?? raw
  }
}

const ARTIFACT_ID = 'tsugumi.ArtifactId'
const SCALAR_SIZE: Partial<Record<BaseType, number>> = {
  [BaseType.bool]: 1, [BaseType.byte]: 1, [BaseType.ubyte]: 1, [BaseType.short]: 2, [BaseType.ushort]: 2, [BaseType.int]: 4, [BaseType.uint]: 4,
  [BaseType.long]: 8, [BaseType.ulong]: 8, [BaseType.float]: 4, [BaseType.double]: 8,
}

/** 16 bytes as lowercase 8-4-4-4-12 uuid text. */
export function uuidText(bytes: Uint8Array): string {
  const hex = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('')
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20, 32)}`
}

/** An artifact as people read it: the uuid with the core's `N.k` label next to it. */
export const artifactName = (id: string, label?: string | null) => label ? `${id} (${label})` : id

/** What an artifact id of a recorded value resolves to now. */
export function artifactStateText(state: D.ArtifactState): string {
  switch (state) {
    case D.ArtifactState.available: return L('利用できます')
    default: return L('利用できません（実行後に残していないか、不明な ID）')
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
  artifacts?: { id: string; sizeBytes: bigint | null; label?: string; state?: D.ArtifactState }[]
  inlineBytes: number[]
  schemaBfbs: number[]
}): string {
  let head = `${v.name} : ${v.rootType}`
  if (v.sizeBytes !== null) head += ` · ${v.sizeBytes} bytes`
  for (const artifact of v.artifacts ?? []) {
    head += '\n' + L('成果物 {0}', artifactName(artifact.id, artifact.label))
    if (artifact.state !== undefined) head += ` · ${artifactStateText(artifact.state)}`
    if (artifact.sizeBytes !== null && artifact.state !== D.ArtifactState.unavailable) head += ` · ${artifact.sizeBytes} bytes`
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

/** A recorded NamedValue with the current state of the artifacts it names. */
export function describeNamedValue(v: D.NamedValueT): string {
  return describeValue({
    name: s(v.name), rootType: s(v.rootType), sizeBytes: v.sizeBytes, inlineBytes: v.inlineBytes, schemaBfbs: v.schemaBfbs,
    artifacts: v.artifacts.map(a => ({ id: s(a.id), sizeBytes: a.sizeBytes, label: s(a.label) || undefined, state: a.state })),
  })
}

function sortKeys(v: Json): Json {
  if (Array.isArray(v)) return v.map(sortKeys)
  if (isObject(v)) return Object.fromEntries(Object.keys(v).sort().map((k) => [k, sortKeys(v[k])]))
  return v
}
