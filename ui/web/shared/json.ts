// JSON value used for the UI's working copy of documents. Object key order
// and the original number spelling are preserved so that 64 bit integers and
// untouched values round-trip exactly (a Map keeps insertion order for every
// key, unlike a plain object whose integer-like keys move to the front).

export class JNum {
  constructor(readonly text: string) {}
}

export class JObj {
  readonly m: Map<string, J>
  constructor(entries: Iterable<[string, J]> = []) {
    this.m = new Map(entries)
  }
  get(k: string): J | undefined {
    return this.m.get(k)
  }
  has(k: string): boolean {
    return this.m.has(k)
  }
  /** Sets a key (appended at the end when new); undefined removes it. */
  set(k: string, v: J | undefined): void {
    if (v === undefined) this.m.delete(k)
    else this.m.set(k, v)
  }
  get size(): number {
    return this.m.size
  }
  keys(): string[] {
    return [...this.m.keys()]
  }
  entries(): [string, J][] {
    return [...this.m.entries()]
  }
}

export type J = null | boolean | string | JNum | J[] | JObj

export const obj = (entries: [string, J][] = []) => new JObj(entries)
export const num = (v: number | bigint | string) => new JNum(String(v))
/** A double that always keeps a fraction (".0") like the Swift UI wrote it. */
export function real(v: number): JNum {
  let s = String(v)
  if (!s.includes('.') && !s.includes('e') && !s.includes('N') && !s.includes('I')) s += '.0'
  return new JNum(s)
}

// ---- Readers ----------------------------------------------------------------

export function get(v: J | undefined, ...keys: string[]): J | undefined {
  let cur = v
  for (const k of keys) {
    if (!(cur instanceof JObj)) return undefined
    cur = cur.get(k)
  }
  return cur
}
export const isObj = (v: J | undefined): v is JObj => v instanceof JObj
export const str = (v: J | undefined): string | undefined => (typeof v === 'string' ? v : undefined)
export const bool = (v: J | undefined): boolean | undefined => (typeof v === 'boolean' ? v : undefined)
export const arr = (v: J | undefined): J[] | undefined => (Array.isArray(v) ? v : undefined)
export const numText = (v: J | undefined): string | undefined => (v instanceof JNum ? v.text : undefined)
export function dbl(v: J | undefined): number | undefined {
  if (!(v instanceof JNum)) return undefined
  const d = Number(v.text)
  return Number.isNaN(d) ? undefined : d
}

// ---- Equality and copies ------------------------------------------------------

export function jeq(a: J | undefined, b: J | undefined): boolean {
  if (a === b) return true
  if (a === undefined || b === undefined || a === null || b === null) return false
  if (a instanceof JNum) return b instanceof JNum && a.text === b.text
  if (Array.isArray(a)) return Array.isArray(b) && a.length === b.length && a.every((x, i) => jeq(x, b[i]))
  if (a instanceof JObj) {
    if (!(b instanceof JObj) || a.size !== b.size) return false
    const ea = a.entries(), eb = b.entries()
    return ea.every(([k, v], i) => eb[i][0] === k && jeq(v, eb[i][1]))
  }
  return false
}

export function clone<T extends J | undefined>(v: T): T {
  if (v instanceof JObj) return new JObj(v.entries().map(([k, x]) => [k, clone(x)])) as T
  if (Array.isArray(v)) return v.map((x) => clone(x)) as T
  return v
}

// ---- Parsing ----------------------------------------------------------------

export class JSONParseError extends Error {
  constructor(message: string, readonly offset: number) {
    super(`${message} at ${offset}`)
  }
}

const NUMBER = /^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$/

export function parse(text: string): J {
  let i = 0
  const n = text.length
  const skip = () => {
    while (i < n) {
      const c = text.charCodeAt(i)
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) i++
      else break
    }
  }
  const string = (): string => {
    i++
    let out = ''
    for (;;) {
      if (i >= n) throw new JSONParseError('unterminated string', i)
      const c = text[i++]
      if (c === '"') return out
      if (c.charCodeAt(0) < 0x20) throw new JSONParseError('control character in string', i)
      if (c !== '\\') {
        out += c
        continue
      }
      const e = text[i++]
      switch (e) {
        case 'n': out += '\n'; break
        case 't': out += '\t'; break
        case 'r': out += '\r'; break
        case 'b': out += '\b'; break
        case 'f': out += '\f'; break
        case 'u': {
          const h = text.slice(i, i + 4)
          if (!/^[0-9a-fA-F]{4}$/.test(h)) throw new JSONParseError('bad \\u escape', i)
          out += String.fromCharCode(parseInt(h, 16))
          i += 4
          break
        }
        default:
          out += e
      }
    }
  }
  const value = (depth: number): J => {
    if (depth > 256) throw new JSONParseError('nesting too deep', i)
    if (i >= n) throw new JSONParseError('unexpected end', i)
    const c = text[i]
    if (c === '{') {
      i++
      const o = new JObj()
      skip()
      if (text[i] === '}') { i++; return o }
      for (;;) {
        skip()
        if (text[i] !== '"') throw new JSONParseError('expected key', i)
        const k = string()
        if (o.has(k)) throw new JSONParseError(`duplicate key ${k}`, i)
        skip()
        if (text[i] !== ':') throw new JSONParseError("expected ':'", i)
        i++
        skip()
        o.set(k, value(depth + 1))
        skip()
        if (text[i] === ',') { i++; continue }
        if (text[i] === '}') { i++; return o }
        throw new JSONParseError("expected ',' or '}'", i)
      }
    }
    if (c === '[') {
      i++
      const a: J[] = []
      skip()
      if (text[i] === ']') { i++; return a }
      for (;;) {
        skip()
        a.push(value(depth + 1))
        skip()
        if (text[i] === ',') { i++; continue }
        if (text[i] === ']') { i++; return a }
        throw new JSONParseError("expected ',' or ']'", i)
      }
    }
    if (c === '"') return string()
    if (text.startsWith('true', i)) { i += 4; return true }
    if (text.startsWith('false', i)) { i += 5; return false }
    if (text.startsWith('null', i)) { i += 4; return null }
    const start = i
    while (i < n && '+-0123456789.eE'.includes(text[i])) i++
    const t = text.slice(start, i)
    if (!NUMBER.test(t)) throw new JSONParseError('invalid number', start)
    return new JNum(t)
  }
  skip()
  const v = value(0)
  skip()
  if (i !== n) throw new JSONParseError('trailing characters', i)
  return v
}

export function tryParse(text: string): J | undefined {
  try {
    return parse(text)
  } catch {
    return undefined
  }
}

// ---- Serialization ------------------------------------------------------------

export function quote(s: string): string {
  let out = '"'
  for (const ch of s) {
    const c = ch.codePointAt(0)!
    if (ch === '"') out += '\\"'
    else if (ch === '\\') out += '\\\\'
    else if (ch === '\n') out += '\\n'
    else if (ch === '\r') out += '\\r'
    else if (ch === '\t') out += '\\t'
    else if (c < 0x20) out += '\\u' + c.toString(16).padStart(4, '0')
    else out += ch
  }
  return out + '"'
}

/** Pretty form with two space indentation (document files are edited by people too). */
export function serialize(v: J, pretty = true): string {
  let out = ''
  const pad = (n: number) => (pretty ? '\n' + '  '.repeat(n) : '')
  const write = (x: J, indent: number) => {
    if (x === null) out += 'null'
    else if (typeof x === 'boolean') out += x ? 'true' : 'false'
    else if (typeof x === 'string') out += quote(x)
    else if (x instanceof JNum) out += x.text
    else if (Array.isArray(x)) {
      if (x.length === 0) { out += '[]'; return }
      out += '['
      x.forEach((e, k) => {
        if (k > 0) out += ','
        out += pad(indent + 1)
        write(e, indent + 1)
      })
      out += pad(indent) + ']'
    } else {
      const es = x.entries()
      if (es.length === 0) { out += '{}'; return }
      out += '{'
      es.forEach(([k, e], j) => {
        if (j > 0) out += ','
        out += pad(indent + 1) + quote(k) + (pretty ? ': ' : ':')
        write(e, indent + 1)
      })
      out += pad(indent) + '}'
    }
  }
  write(v, 0)
  if (pretty) out += '\n'
  return out
}
