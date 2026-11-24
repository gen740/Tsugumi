export class TrapError extends Error {
  name = 'TrapError'
}

export class Handles {
  #next = 1
  #entries = new Map()

  add(kind, object) {
    if (object == null) return 0
    if (this.#next > 0xffffffff) throw new TrapError('WebGPU handle ids exhausted')
    const id = this.#next++
    this.#entries.set(id, { kind, object })
    return id
  }

  get(kind, id) {
    id >>>= 0
    const entry = this.#entries.get(id)
    if (!entry) throw new TrapError(`unknown WebGPU handle id ${id}`)
    if (entry.kind !== kind) throw new TrapError(`wrong kind for WebGPU handle id ${id}`)
    return entry.object
  }

  take(kind, id) {
    const object = this.get(kind, id)
    this.#entries.delete(id >>> 0)
    return object
  }

  takeIfPresent(kind, id) {
    return this.#entries.has(id >>> 0) ? this.take(kind, id) : undefined
  }
}

const NOT_USED = Symbol('bindingNotUsed')
const decoder = new TextDecoder('utf-8', { ignoreBOM: true })
const U64_MAX = 0xffffffffffffffffn

class Reader {
  #view

  constructor(ctx, name) {
    this.ctx = ctx
    this.name = name
  }

  fail(field, reason) {
    throw new TrapError(`${this.name}: ${field}: ${reason}`)
  }

  get view() {
    return this.#view ??= new DataView(this.ctx.memory())
  }

  range(address, size, field) {
    if (address > this.view.byteLength || size > this.view.byteLength - address) {
      this.fail(field, 'out of bounds')
    }
  }

  required(address, size, field) {
    if (address === 0) this.fail(field, 'null pointer')
    this.range(address, size, field)
  }

  u16(address, field) {
    this.range(address, 2, field)
    return this.view.getUint16(address, true)
  }

  u32(address, field) {
    this.range(address, 4, field)
    return this.view.getUint32(address, true)
  }

  i32(address, field) {
    return this.u32(address, field) | 0
  }

  u64(address, field) {
    this.range(address, 8, field)
    return this.view.getBigUint64(address, true)
  }

  f32(address, field) {
    this.range(address, 4, field)
    return this.view.getFloat32(address, true)
  }

  f64(address, field) {
    this.range(address, 8, field)
    return this.view.getFloat64(address, true)
  }

  number(value, field) {
    if (value > BigInt(Number.MAX_SAFE_INTEGER)) this.fail(field, 'integer exceeds JS precision')
    return Number(value)
  }

  enum(values, value, field) {
    if (!Object.hasOwn(values, value)) this.fail(field, `unknown enum value ${value}`)
    return values[value]
  }

  flags(mask, value, field) {
    if (value & ~mask) this.fail(field, 'unknown bitflag bits')
    return this.number(value, field)
  }

  handle(kind, id, optional, field) {
    if (optional && id === 0) return null
    try {
      return this.ctx.handles.get(kind, id)
    } catch (error) {
      if (error instanceof TrapError) this.fail(field, error.message)
      throw error
    }
  }

  take(kind, id, field) {
    try {
      return this.ctx.handles.take(kind, id)
    } catch (error) {
      if (error instanceof TrapError) this.fail(field, error.message)
      throw error
    }
  }

  string(address, length, field) {
    if (address === 0) {
      if (length === 0xffffffff) return undefined
      if (length === 0) return ''
      this.fail(field, 'null string data')
    }
    if (length === 0xffffffff) {
      this.range(address, 0, field)
      const bytes = new Uint8Array(this.view.buffer, address)
      length = bytes.indexOf(0)
      if (length === -1) this.fail(field, 'unterminated string')
      return decoder.decode(bytes.subarray(0, length))
    }
    this.range(address, length, field)
    return decoder.decode(new Uint8Array(this.view.buffer, address, length))
  }

  data(address, size, field) {
    if (address === 0 && size !== 0) this.fail(field, 'null pointer')
    this.range(address, size, field)
    return new Uint8Array(this.view.buffer, address, size)
  }

  array(address, count, stride, field, decode) {
    if (count === 0) {
      this.range(address, 0, field)
      return []
    }
    this.required(address, count * stride, field)
    return Array.from({ length: count }, (_, index) => decode(address + index * stride))
  }
}
