// Code editor: monospaced, syntax colors, line numbers, indentation from the
// settings, guides and markers for errors. A transparent textarea lies over
// the colored text, so typing, selection, undo and input methods stay the
// browser's own. Undo belongs to one document: switching documents never
// carries it (or the selection) over.
import { CSSProperties, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { L } from '../i18n'

/** Token classes the editor colors. */
export type SyntaxToken = 'plain' | 'comment' | 'string' | 'number' | 'keyword' | 'function' | 'type' | 'constant' | 'property'
const TOKENS: SyntaxToken[] = ['plain', 'comment', 'string', 'number', 'keyword', 'function', 'type', 'constant', 'property']

/** One problem located in the edited text (byte offset into UTF-8). */
export interface EditorMarker {
  offset: number | bigint
  message: string
}

// ---- Highlighting ------------------------------------------------------------------

interface Rules {
  /** Comments and strings, scanned left to right (the earliest match wins;
   *  nothing else is colored inside them). Patterns have no capture groups. */
  claim: [string, SyntaxToken][]
  /** Colored after the claims; later rules win over earlier ones. */
  paint: [string, SyntaxToken, number?][]
  keywords?: string[]
  flags?: string
}

const words = (list: string[]) => `\\b(?:${list.join('|')})\\b`
const NUMBER = String.raw`\b(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\b`
const CALL = String.raw`\b([A-Za-z_][A-Za-z0-9_]*)\s*\(`
const DQ = String.raw`"(?:\\.|[^"\\\n])*"`

const CPP_TYPES = ['bool', 'char', 'double', 'float', 'int', 'long', 'short', 'signed', 'unsigned', 'void', 'size_t', 'auto',
  'int8_t', 'int16_t', 'int32_t', 'int64_t', 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t']
const CPP_KEYWORDS = ['break', 'case', 'catch', 'class', 'const', 'constexpr', 'continue', 'default', 'delete', 'do', 'else', 'enum', 'explicit',
  'for', 'if', 'inline', 'namespace', 'new', 'noexcept', 'operator', 'override', 'private', 'protected', 'public', 'return', 'static',
  'struct', 'switch', 'template', 'this', 'throw', 'try', 'typename', 'using', 'virtual', 'while']
const PY_KEYWORDS = ['and', 'as', 'assert', 'async', 'await', 'break', 'class', 'continue', 'def', 'del', 'elif', 'else', 'except', 'finally',
  'for', 'from', 'global', 'if', 'import', 'in', 'is', 'lambda', 'nonlocal', 'not', 'or', 'pass', 'raise', 'return', 'try', 'while',
  'with', 'yield', 'self', 'cls']
const NIX_KEYWORDS = ['assert', 'else', 'if', 'import', 'in', 'inherit', 'let', 'rec', 'then', 'with']
const FBS_KEYWORDS = ['attribute', 'enum', 'file_identifier', 'file_extension', 'include', 'namespace', 'root_type', 'rpc_service', 'struct', 'table', 'union']
const FBS_TYPES = ['bool', 'byte', 'ubyte', 'short', 'ushort', 'int', 'uint', 'long', 'ulong', 'float', 'double', 'string',
  'int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'float32', 'float64']
const SH_KEYWORDS = ['case', 'do', 'done', 'echo', 'elif', 'else', 'esac', 'export', 'fi', 'for', 'function', 'if', 'in', 'local', 'return',
  'set', 'then', 'while', 'until']

function rules(language: string): Rules | undefined {
  switch (language.toLowerCase()) {
    case 'cpp': case 'cc': case 'hpp': case 'h': case 'hh': case 'cxx': case 'c':
      return {
        claim: [[String.raw`//[^\n]*`, 'comment'], [String.raw`/\*[\s\S]*?\*/`, 'comment'], [DQ, 'string'], [String.raw`'(?:\\.|[^'\\\n])'`, 'string']],
        paint: [[NUMBER, 'number'], [words(CPP_TYPES), 'type'], [String.raw`\b(?:struct|class|enum|namespace)\s+([A-Za-z_]\w*)`, 'type', 1],
          [String.raw`\b([A-Za-z_]\w*)::`, 'type', 1], [words(['true', 'false', 'nullptr', 'NULL']), 'constant'], [words(CPP_KEYWORDS), 'keyword'],
          [String.raw`^[ \t]*#[ \t]*\w+`, 'keyword'], [CALL, 'function', 1], [String.raw`(?:\.|->)([A-Za-z_]\w*)\b(?!\s*\()`, 'property', 1]],
        keywords: CPP_KEYWORDS, flags: 'm',
      }
    case 'py': case 'python':
      return {
        claim: [[String.raw`#[^\n]*`, 'comment'], [String.raw`[rRbBfFuU]{0,2}"""[\s\S]*?"""`, 'string'], [String.raw`[rRbBfFuU]{0,2}'''[\s\S]*?'''`, 'string'],
          [String.raw`[rRbBfFuU]{0,2}"(?:\\.|[^"\\\n])*"`, 'string'], [String.raw`[rRbBfFuU]{0,2}'(?:\\.|[^'\\\n])*'`, 'string']],
        paint: [[NUMBER, 'number'], [words(['True', 'False', 'None']), 'constant'], [words(PY_KEYWORDS), 'keyword'], [CALL, 'function', 1],
          [String.raw`\bdef\s+([A-Za-z_]\w*)`, 'function', 1], [String.raw`\bclass\s+([A-Za-z_]\w*)`, 'type', 1], [String.raw`^[ \t]*@[\w.]+`, 'function'],
          [String.raw`\.([A-Za-z_]\w*)\b(?!\s*\()`, 'property', 1]],
        keywords: PY_KEYWORDS, flags: 'm',
      }
    case 'nix':
      return {
        claim: [[String.raw`#[^\n]*`, 'comment'], [String.raw`/\*[\s\S]*?\*/`, 'comment'], [String.raw`''[\s\S]*?''`, 'string'], [String.raw`"(?:\\.|[^"\\])*"`, 'string'],
          [String.raw`(?:\.\.?|~)?/[\w.+-]+(?:/[\w.+-]+)*`, 'string']],
        paint: [[NUMBER, 'number'], [words(['true', 'false', 'null']), 'constant'], [words(NIX_KEYWORDS), 'keyword'],
          [String.raw`([A-Za-z_][\w'-]*(?:\.[A-Za-z_][\w'-]*)*)\s*=(?!=)`, 'property', 1], [String.raw`\bbuiltins\b`, 'keyword']],
        keywords: NIX_KEYWORDS,
      }
    case 'fbs':
      return {
        claim: [[String.raw`//[^\n]*`, 'comment'], [DQ, 'string']],
        paint: [[NUMBER, 'number'], [words(['true', 'false']), 'constant'], [String.raw`^[ \t]*([A-Za-z_]\w*)\s*:`, 'property', 1],
          [String.raw`:\s*\[?\s*([A-Za-z_][\w.]*)`, 'type', 1], [String.raw`\b(?:table|struct|enum|union|root_type)\s+([A-Za-z_][\w.]*)`, 'type', 1],
          [words(FBS_TYPES), 'type'], [words(FBS_KEYWORDS), 'keyword'], [String.raw`\(\s*([A-Za-z_]\w*)`, 'property', 1],
          [String.raw`,\s*([A-Za-z_]\w*)(?=[^()]*\))`, 'property', 1]],
        keywords: FBS_KEYWORDS, flags: 'm',
      }
    case 'json':
      return {
        claim: [[String.raw`"(?:\\.|[^"\\\n])*"(?=\s*:)`, 'property'], [DQ, 'string']],
        paint: [[String.raw`-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b`, 'number'], [words(['true', 'false', 'null']), 'constant']],
      }
    case 'sh': case 'bash':
      return {
        claim: [[String.raw`(?:^|[ \t])#[^\n]*`, 'comment'], [String.raw`"(?:\\.|[^"\\])*"`, 'string'], [String.raw`'[^']*'`, 'string']],
        paint: [[NUMBER, 'number'], [words(SH_KEYWORDS), 'keyword'], [String.raw`\$\{?[A-Za-z_]\w*\}?|\$[0-9@#?*!$-]`, 'property']],
        keywords: SH_KEYWORDS, flags: 'm',
      }
    default:
      return undefined
  }
}

const compiled = new Map<string, { claim: RegExp; claimTokens: SyntaxToken[]; paint: [RegExp, SyntaxToken, number][]; keywords: Set<string> } | null>()

function compile(language: string) {
  const key = language.toLowerCase()
  if (compiled.has(key)) return compiled.get(key)!
  const r = rules(key)
  const c = r ? {
    claim: new RegExp(r.claim.map(([p]) => `(${p})`).join('|'), 'g' + (r.flags ?? '')),
    claimTokens: r.claim.map(([, t]) => t),
    paint: r.paint.map(([p, t, g]) => [new RegExp(p, 'gd' + (r.flags ?? '')), t, g ?? 0] as [RegExp, SyntaxToken, number]),
    keywords: new Set(r.keywords ?? []),
  } : null
  compiled.set(key, c)
  return c
}

/** Token per UTF-16 unit of `text` (indexes into TOKENS), or undefined when
 *  the language has no rules. Texts here are Card sized. */
function tokenize(text: string, language: string): Uint8Array | undefined {
  const c = compile(language)
  if (!c || text.length > 400_000) return undefined
  const out = new Uint8Array(text.length)
  const claimed = new Uint8Array(text.length)
  c.claim.lastIndex = 0
  for (let m = c.claim.exec(text); m; m = c.claim.exec(text)) {
    if (m[0].length === 0) {
      c.claim.lastIndex++
      continue
    }
    const which = m.slice(1).findIndex((g) => g !== undefined)
    const t = TOKENS.indexOf(c.claimTokens[which] ?? 'plain')
    out.fill(t, m.index, m.index + m[0].length)
    claimed.fill(1, m.index, m.index + m[0].length)
  }
  for (const [re, token, group] of c.paint) {
    const t = TOKENS.indexOf(token)
    re.lastIndex = 0
    for (let m = re.exec(text); m; m = re.exec(text)) {
      if (m[0].length === 0) {
        re.lastIndex++
        continue
      }
      const g = m[group]
      if (g === undefined) continue
      if (token === 'function' && c.keywords.has(g)) continue
      const start = m.indices?.[group]?.[0] ?? m.index
      for (let i = start; i < start + g.length; i++) if (!claimed[i]) out[i] = t
    }
  }
  return out
}

// ---- Offsets -----------------------------------------------------------------------

/** UTF-16 index of a UTF-8 byte offset. */
function utf16Index(text: string, byteOffset: number): number {
  let bytes = 0
  for (let i = 0; i < text.length; i++) {
    if (bytes >= byteOffset) return i
    const cp = text.codePointAt(i)!
    bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4
    if (cp > 0xffff) i++
  }
  return text.length
}

const lineOf = (text: string, index: number): number => {
  let n = 1
  for (let i = 0; i < index && i < text.length; i++) if (text.charCodeAt(i) === 10) n++
  return n
}

// ---- Editor ------------------------------------------------------------------------

/** Selection and scroll of each document seen (kept across switches). */
const places = new Map<string, { start: number; end: number; top: number; left: number }>()

export function CodeEditor({ text, onChange, language = '', editable = true, markers = [], documentId = '', style, className }: {
  text: string
  onChange?: (t: string) => void
  /** File extension or language name. */
  language?: string
  editable?: boolean
  markers?: EditorMarker[]
  /** Which document is shown (a file path). */
  documentId?: string
  style?: CSSProperties
  className?: string
}) {
  useObserve(prefs)
  const ta = useRef<HTMLTextAreaElement>(null)
  const scroll = useRef<HTMLDivElement>(null)
  const typed = useRef<string | undefined>(undefined)
  const [cursorLine, setCursorLine] = useState(1)
  const indentWidth = Math.max(1, prefs.indentWidth)
  const fontSize = prefs.editorFontSize
  const lh = Math.round(fontSize * 1.45)

  const tokens = useMemo(() => tokenize(text, language), [text, language])
  const lines = useMemo(() => text.split('\n'), [text])
  const problems = useMemo(() => {
    const out = new Map<number, string>()
    for (const m of markers) {
      const line = lineOf(text, utf16Index(text, Number(m.offset)))
      out.set(line, out.has(line) ? `${out.get(line)}\n${m.message}` : m.message)
    }
    return out
  }, [markers, text])

  const remember = () => {
    const el = ta.current, sc = scroll.current
    if (!el || !sc) return
    places.set(documentId, { start: el.selectionStart, end: el.selectionEnd, top: sc.scrollTop, left: sc.scrollLeft })
    setCursorLine(lineOf(el.value, el.selectionStart))
  }

  // Another document: its own place (or the start), never the previous one's.
  useLayoutEffect(() => {
    const el = ta.current, sc = scroll.current
    if (!el || !sc) return
    typed.current = text
    const p = places.get(documentId)
    const len = el.value.length
    el.setSelectionRange(Math.min(p?.start ?? 0, len), Math.min(p?.end ?? 0, len))
    sc.scrollTop = p?.top ?? 0
    sc.scrollLeft = p?.left ?? 0
    setCursorLine(lineOf(el.value, el.selectionStart))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [documentId])

  // Text replaced from outside (formatting, a reload): the selection stays where it was.
  useLayoutEffect(() => {
    const el = ta.current
    if (!el || typed.current === text) return
    typed.current = text
    const p = places.get(documentId)
    if (p) el.setSelectionRange(Math.min(p.start, text.length), Math.min(p.end, text.length))
  }, [text, documentId])

  const insert = (s: string) => {
    // execCommand keeps the edit in the textarea's own undo history.
    if (!document.execCommand('insertText', false, s)) {
      const el = ta.current!
      const { selectionStart: a, selectionEnd: b } = el
      onChange?.(el.value.slice(0, a) + s + el.value.slice(b))
    }
  }

  const gutterTip = [...problems].sort((a, b) => a[0] - b[0]).map(([l, m]) => `${l}: ${m}`).join('\n')
  const lineHeight = `${lh}px`
  let offset = 0
  return (
    <div className={`code-editor ce ce-theme-${prefs.editorTheme} ${className ?? ''}`} style={{ fontSize, ...style }}>
      <div className="ce-scroll" ref={scroll} onScroll={remember}>
        <div className="ce-body">
          <div className="ce-gutter" title={gutterTip || undefined} aria-hidden>
            {lines.map((_, i) => {
              const n = i + 1
              const marked = problems.has(n)
              return (
                <div key={i} className={`ce-num${marked ? ' marked' : n === cursorLine ? ' current' : ''}`} style={{ height: lineHeight, lineHeight }}>
                  {marked && <span className="ce-dot" />}{n}
                </div>
              )
            })}
          </div>
          <div className="ce-stack">
            <pre className="ce-hl" aria-hidden style={{ lineHeight }}>
              {lines.map((line, i) => {
                const start = offset
                offset += line.length + 1
                const lead = line.match(/^ */)![0].length
                const levels = prefs.showIndentGuides ? Math.floor(lead / indentWidth) : 0
                return (
                  <div key={i} className={`ce-line${problems.has(i + 1) ? ' marked' : ''}`} style={{ height: lineHeight }}>
                    {Array.from({ length: levels }, (_, k) => <span key={'g' + k} className="ce-guide" style={{ left: `calc(${k * indentWidth}ch + 1px)` }} />)}
                    {segments(line, start, tokens)}
                    {line.length === 0 && '​'}
                  </div>
                )
              })}
            </pre>
            <textarea key={documentId} ref={ta} className="ce-input" value={text} readOnly={!editable} rows={1} cols={1}
              spellCheck={false} autoCapitalize="off" autoCorrect="off" autoComplete="off" wrap="off" style={{ lineHeight }}
              aria-label={documentId || L('コード')}
              onChange={(e) => {
                typed.current = e.target.value
                onChange?.(e.target.value)
              }}
              onSelect={remember} onKeyUp={remember} onMouseUp={remember}
              onKeyDown={(e) => {
                if (!editable || e.nativeEvent.isComposing) return
                if (e.key === 'Tab' && !e.shiftKey && !e.metaKey && !e.ctrlKey && !e.altKey) {
                  e.preventDefault()
                  insert(' '.repeat(indentWidth))
                } else if (e.key === 'Enter' && !e.metaKey && !e.ctrlKey && !e.altKey && !e.shiftKey) {
                  // Keep the indentation of the current line, one more after a block opener.
                  const el = e.currentTarget
                  const start = el.value.lastIndexOf('\n', el.selectionStart - 1) + 1
                  const end = el.value.indexOf('\n', el.selectionStart)
                  const current = el.value.slice(start, end < 0 ? undefined : end)
                  const lead = current.match(/^[ \t]*/)?.[0] ?? ''
                  const t = current.trim()
                  const extra = t.endsWith(':') || t.endsWith('{') || t.endsWith('[') || t.endsWith('(') ? ' '.repeat(indentWidth) : ''
                  e.preventDefault()
                  insert('\n' + lead + extra)
                }
              }} />
          </div>
        </div>
      </div>
    </div>
  )
}

/** Colored runs of one line. */
function segments(line: string, start: number, tokens: Uint8Array | undefined) {
  if (!tokens || !line) return line
  const out: JSX.Element[] = []
  let i = 0
  while (i < line.length) {
    const t = tokens[start + i]
    let j = i + 1
    while (j < line.length && tokens[start + j] === t) j++
    const s = line.slice(i, j)
    out.push(t ? <span key={i} className={`tok-${TOKENS[t]}`}>{s}</span> : <span key={i}>{s}</span>)
    i = j
  }
  return out
}
