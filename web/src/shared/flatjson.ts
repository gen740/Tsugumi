// Converts between JSON values and FlatBuffers tables using a reflection
// schema. Unions use the flatc JSON convention (`<field>_type` + `<field>`).
// 64 bit integers are decimal strings in JSON beyond 2^53.
import { Builder, ByteBuffer } from 'flatbuffers'
import { BaseType, BFBS, Field } from './bfbs'
import { L } from '../i18n'

export class InvalidArguments extends Error {}

type Json = unknown
type JsonObject = Record<string, Json>

const MAX_SAFE = 9007199254740991n
/** Element sizes of the scalar vectors built from JSON arrays. */
const SIZES: Partial<Record<BaseType, number>> = {
  [BaseType.bool]: 1, [BaseType.short]: 2, [BaseType.ushort]: 2, [BaseType.int]: 4, [BaseType.uint]: 4,
  [BaseType.float]: 4, [BaseType.long]: 8, [BaseType.ulong]: 8, [BaseType.double]: 8,
}
const isObject = (v: Json): v is JsonObject => typeof v === 'object' && v !== null && !Array.isArray(v)

const RANGES: Partial<Record<BaseType, [bigint, bigint]>> = {
  [BaseType.byte]: [-128n, 127n],
  [BaseType.ubyte]: [0n, 255n],
  [BaseType.short]: [-32768n, 32767n],
  [BaseType.ushort]: [0n, 65535n],
  [BaseType.int]: [-2147483648n, 2147483647n],
  [BaseType.uint]: [0n, 4294967295n],
  [BaseType.long]: [-9223372036854775808n, 9223372036854775807n],
  [BaseType.ulong]: [0n, 18446744073709551615n],
}

export class FlatJSON {
  constructor(readonly schema: BFBS) {}

  // ---- JSON -> FlatBuffer ------------------------------------------------------

  /** Builds a table of `object` from `json`. Unknown keys and type errors are
   *  rejected so incomplete tool arguments never reach the core. */
  build(object: number, json: JsonObject, b: Builder, path = ''): number {
    const obj = this.schema.objects[object]
    const known = new Set(obj.fields.map((f) => f.name))
    for (const k of Object.keys(json)) {
      if (!known.has(k)) throw new InvalidArguments(`${path}${k} is not a field of ${obj.name.split('.').pop()}`)
    }
    // Children first: FlatBuffers needs nested objects before the table starts.
    const offsets = new Map<number, number>()
    for (const f of obj.fields) {
      const v = json[f.name]
      if (v === undefined || v === null) continue
      const where = path + f.name
      switch (f.type.base) {
        case BaseType.string:
          if (typeof v !== 'string') throw new InvalidArguments(`${where} must be a string`)
          offsets.set(f.id, b.createString(v))
          break
        case BaseType.obj:
          if (!isObject(v)) throw new InvalidArguments(`${where} must be an object`)
          offsets.set(f.id, this.build(f.type.index, v, b, where + '.'))
          break
        case BaseType.union: {
          if (!isObject(v)) throw new InvalidArguments(`${where} must be an object`)
          const variant = this.unionVariant(f, json[`${f.name}_type`], where)
          offsets.set(f.id, this.build(variant.unionType!.index, v, b, where + '.'))
          break
        }
        case BaseType.vector:
          offsets.set(f.id, this.vector(f, v, b, where))
          break
      }
    }
    const slots = Math.max(0, ...obj.fields.map((f) => f.id)) + 1
    b.startObject(slots)
    for (const f of obj.fields) {
      const off = offsets.get(f.id)
      if (off !== undefined) {
        b.addFieldOffset(f.id, off, 0)
        continue
      }
      if (f.type.base === BaseType.utype) {
        const unionName = f.name.slice(0, -5) // "<name>_type"
        const uf = obj.fields.find((x) => x.name === unionName)
        if (json[unionName] === undefined || !uf) continue
        b.addFieldInt8(f.id, this.unionVariant(uf, json[f.name], path + f.name).value, null)
        continue
      }
      const v = json[f.name]
      if (v === undefined || v === null) continue
      this.scalar(f, v, b, path + f.name)
    }
    return b.endObject()
  }

  private unionVariant(f: Field, name: Json, where: string) {
    const e = this.schema.enums[f.type.index]
    if (typeof name !== 'string') {
      throw new InvalidArguments(`${where}_type is required (one of ${e.values.slice(1).map((v) => v.name).join(', ')})`)
    }
    const v = e.values.find((x) => x.name === name && x.value !== 0)
    if (!v) throw new InvalidArguments(`${where}_type ${name} is unknown`)
    return v
  }

  private integer(f: Field, base: BaseType, v: Json, where: string): bigint {
    if (f.type.index >= 0 && typeof v === 'string' && base !== BaseType.long && base !== BaseType.ulong) {
      const ev = this.schema.enums[f.type.index].values.find((x) => x.name === v)
      if (!ev) throw new InvalidArguments(`${where} must be one of ${this.schema.enums[f.type.index].values.map((x) => x.name).join(', ')}`)
      return BigInt(ev.value)
    }
    let x: bigint
    if (typeof v === 'string') {
      if (!/^-?[0-9]+$/.test(v)) throw new InvalidArguments(`${where} must be a decimal integer`)
      x = BigInt(v)
    } else if (typeof v === 'number') {
      if (!Number.isInteger(v) || Math.abs(v) > Number(MAX_SAFE)) {
        throw new InvalidArguments(`${where} must be an integer (use a decimal string for 64 bit values)`)
      }
      x = BigInt(v)
    } else {
      throw new InvalidArguments(`${where} must be an integer`)
    }
    const r = RANGES[base]
    if (r && (x < r[0] || x > r[1])) throw new InvalidArguments(`${where} is out of range`)
    return x
  }

  private scalar(f: Field, v: Json, b: Builder, where: string) {
    const base = f.type.base
    switch (base) {
      case BaseType.bool:
        if (typeof v !== 'boolean') throw new InvalidArguments(`${where} must be true or false`)
        b.addFieldInt8(f.id, v ? 1 : 0, null)
        return
      case BaseType.byte:
      case BaseType.ubyte:
        b.addFieldInt8(f.id, Number(this.integer(f, base, v, where)), null)
        return
      case BaseType.short:
      case BaseType.ushort:
        b.addFieldInt16(f.id, Number(this.integer(f, base, v, where)), null)
        return
      case BaseType.int:
      case BaseType.uint:
        b.addFieldInt32(f.id, Number(this.integer(f, base, v, where)), null)
        return
      case BaseType.long:
      case BaseType.ulong:
        b.addFieldInt64(f.id, BigInt.asIntN(64, this.integer(f, base, v, where)), null)
        return
      case BaseType.float:
      case BaseType.double:
        if (typeof v !== 'number' || !Number.isFinite(v)) throw new InvalidArguments(`${where} must be a finite number`)
        if (base === BaseType.float) b.addFieldFloat32(f.id, v, null)
        else b.addFieldFloat64(f.id, v, null)
        return
      default:
        throw new InvalidArguments(`${where} has an unsupported type`)
    }
  }

  private vector(f: Field, v: Json, b: Builder, where: string): number {
    const el = f.type.element
    if (el === BaseType.ubyte || el === BaseType.byte) {
      // Byte vectors carry text (UTF-8) or explicit {"base64": "..."}.
      if (typeof v === 'string') return b.createByteVector(new TextEncoder().encode(v))
      if (isObject(v) && typeof v.base64 === 'string') return b.createByteVector(fromBase64(v.base64))
      throw new InvalidArguments(`${where} must be text or {"base64": ...}`)
    }
    if (!Array.isArray(v)) throw new InvalidArguments(`${where} must be an array`)
    if (el === BaseType.string) {
      const offs = v.map((s) => {
        if (typeof s !== 'string') throw new InvalidArguments(`${where} must be an array of strings`)
        return b.createString(s)
      })
      return offsetVector(b, offs)
    }
    if (el === BaseType.obj) {
      const offs = v.map((o, i) => {
        if (!isObject(o)) throw new InvalidArguments(`${where} must be an array of objects`)
        return this.build(f.type.index, o, b, `${where}[${i}].`)
      })
      return offsetVector(b, offs)
    }
    const size = SIZES[el]
    if (!size) throw new InvalidArguments(`${where} has an unsupported element type`)
    b.startVector(size, v.length, size)
    const plain = { ...f, type: { ...f.type, index: -1 } }
    for (let i = v.length - 1; i >= 0; i--) {
      const x = v[i]
      if (el === BaseType.double || el === BaseType.float) {
        if (typeof x !== 'number' || !Number.isFinite(x)) throw new InvalidArguments(`${where} must be numbers`)
        if (el === BaseType.double) b.addFloat64(x)
        else b.addFloat32(x)
      } else if (el === BaseType.bool) {
        if (typeof x !== 'boolean') throw new InvalidArguments(`${where} must be true or false`)
        b.addInt8(x ? 1 : 0)
      } else {
        const n = this.integer(plain, el, x, where)
        if (size === 8) b.addInt64(BigInt.asIntN(64, n))
        else if (size === 4) b.addInt32(Number(BigInt.asIntN(32, n)))
        else b.addInt16(Number(BigInt.asIntN(16, n)))
      }
    }
    return b.endVector()
  }

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

function offsetVector(b: Builder, offs: number[]): number {
  b.startVector(4, offs.length, 4)
  for (let i = offs.length - 1; i >= 0; i--) b.addOffset(offs[i])
  return b.endVector()
}

const big = (v: bigint): Json => (v > MAX_SAFE || v < -MAX_SAFE ? v.toString() : Number(v))

export function toBase64(bytes: Uint8Array): string {
  let s = ''
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000))
  return btoa(s)
}

export function fromBase64(s: string): Uint8Array {
  const bin = atob(s)
  const out = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i)
  return out
}

/** A recorded value (inline FlatBuffer with its own BFBS) as pretty JSON. */
export function describeValue(v: {
  name: string
  rootType: string
  sizeBytes: bigint | null
  artifactId: string | null
  filePath?: string | null
  fileMissing?: boolean
  inlineBytes: number[]
  schemaBfbs: number[]
}): string {
  let head = `${v.name} : ${v.rootType}`
  if (v.sizeBytes !== null) head += ` · ${v.sizeBytes} bytes`
  if (v.artifactId) head += L(' · 成果物 {0}', v.artifactId)
  if (v.fileMissing) head += L('\n保持したファイルの内容がありません（scratch は保持履歴ではありません）。')
  else if (v.filePath) head += L('\n保持したファイル: {0}', v.filePath)
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
