// Markdown with TeX math, as the macOS app's MarkdownView draws it: blocks
// (paragraphs, headings, code, displayed math, quotes, lists with check
// boxes, tables, rules) and inlines (emphasis, strong, strike, code, links,
// inline math). `$…$` is inline math, `$$…$$` a displayed formula; TeX that
// cannot be typeset stays as written. Math is taken out before parsing, so
// TeX is never read as emphasis or escapes. Sizes follow the font size of
// the surrounding element (a canvas zoom lays it out again, never scaled).
import katex from 'katex'
import { marked, Token, Tokens } from 'marked'
import type { ReactNode } from 'react'

const OPEN = '', CLOSE = ''
const PLACEHOLDER = /(\d+)/g

/** Replaces `$$…$$` and `$…$` outside code with placeholders; the third
 *  value holds the indices of the displayed (`$$…$$`) ones. */
export function maskMath(text: string): [string, string[], Set<number>] {
  let out = ''
  const math: string[] = []
  const displayed = new Set<number>()
  const chars = Array.from(text)
  let i = 0
  let fence: string | undefined
  let lineStart = true
  const placeholder = (tex: string) => {
    math.push(tex)
    return `${OPEN}${math.length - 1}${CLOSE}`
  }
  const find = (s: string, from: number) => {
    const p = Array.from(s)
    for (let j = from; j + p.length <= chars.length; j++) if (p.every((c, n) => chars[j + n] === c)) return j
    return -1
  }
  while (i < chars.length) {
    const c = chars[i]
    if (lineStart) {
      // Fenced code blocks are left alone.
      let j = i
      while (j < chars.length && chars[j] === ' ') j++
      const rest = chars.slice(j, j + 3).join('')
      if (rest === '```' || rest === '~~~') {
        if (fence === undefined) fence = rest
        else if (fence === rest) fence = undefined
        while (i < chars.length && chars[i] !== '\n') out += chars[i++]
        continue
      }
    }
    lineStart = c === '\n'
    if (fence !== undefined) {
      out += c
      i++
      continue
    }
    if (c === '\\' && chars[i + 1] === '$') {
      out += '\\$'
      i += 2
      continue
    }
    if (c === '`') {
      // Inline code: copied up to the matching run of backticks.
      let n = 0
      while (chars[i + n] === '`') n++
      const tick = '`'.repeat(n)
      const found = find(tick, i + n)
      const end = found >= 0 ? found + n : i + n
      out += chars.slice(i, end).join('')
      i = end
      continue
    }
    if (c === '$' && chars[i + 1] === '$') {
      const e = find('$$', i + 2)
      if (e >= 0) {
        displayed.add(math.length)
        out += '\n\n' + placeholder(chars.slice(i + 2, e).join('').trim()) + '\n\n'
        i = e + 2
        continue
      }
    }
    if (c === '$' && i + 1 < chars.length && chars[i + 1] !== ' ' && chars[i + 1] !== '\n') {
      let j = i + 1
      while (j < chars.length && chars[j] !== '$' && chars[j] !== '\n') j++
      // "$5 and $6" is money, not math: a closing $ followed by a digit is not one.
      if (j < chars.length && chars[j] === '$' && chars[j - 1] !== ' ' && j > i + 1 && !/\p{Nd}/u.test(chars[j + 1] ?? '')) {
        out += placeholder(chars.slice(i + 1, j).join(''))
        i = j + 1
        continue
      }
    }
    out += c
    i++
  }
  return [out, math, displayed]
}

/** Common commands written with the basic ones (as the app's typesetter needs;
 *  KaTeX has them, the rewrite keeps the two looking the same). */
export function supportedTeX(tex: string): string {
  const rules: [RegExp, string][] = [
    [/\\pmod\s*\{([^{}]*)\}/g, '\\ (\\mathrm{mod}\\ $1)'],
    [/\\pmod\s*([A-Za-z0-9])/g, '\\ (\\mathrm{mod}\\ $1)'],
    [/\\bmod(?![A-Za-z])/g, '\\ \\mathrm{mod}\\ '],
    [/\\qquad(?![A-Za-z])/g, '\\ \\ \\ \\ \\ \\ \\ \\ '],
    [/\\quad(?![A-Za-z])/g, '\\ \\ \\ \\ '],
  ]
  return rules.reduce((t, [p, r]) => t.replace(p, r), tex)
}

const mathCache = new Map<string, string | null>()

/** MathML for a formula (drawn by the browser, resolution independent), or
 *  null when it cannot be typeset. */
function typeset(tex: string, display: boolean): string | null {
  const k = (display ? 'D|' : 'T|') + tex
  const hit = mathCache.get(k)
  if (hit !== undefined) return hit
  let made: string | null = null
  try {
    made = katex.renderToString(supportedTeX(tex), { displayMode: display, output: 'mathml', throwOnError: true, strict: 'ignore', trust: false })
  } catch {
    made = null
  }
  if (mathCache.size > 2000) mathCache.clear()
  mathCache.set(k, made)
  return made
}

interface Doc {
  tokens: Token[]
  math: string[]
  displayed: Set<number>
}

const docCache = new Map<string, Doc>()

function parse(text: string): Doc {
  const hit = docCache.get(text)
  if (hit) return hit
  const [masked, math, displayed] = maskMath(text)
  const d = { tokens: marked.lexer(masked, { gfm: true }), math, displayed }
  if (docCache.size > 500) docCache.clear()
  docCache.set(text, d)
  return d
}

function InlineMath({ tex }: { tex: string }) {
  const m = typeset(tex, false)
  if (m === null) return <code className="md-tex">{'$' + tex + '$'}</code>
  return <span className="md-math" dangerouslySetInnerHTML={{ __html: m }} />
}

function MathBlock({ tex }: { tex: string }) {
  const m = typeset(tex, true)
  if (m === null) return <pre className="md-tex md-math-block">{'$$' + tex + '$$'}</pre>
  return <div className="md-math-block" aria-label={tex} dangerouslySetInnerHTML={{ __html: m }} />
}

const safeHref = (href: string) => (/^(https?:|mailto:)/i.test(href) ? href : undefined)

type CodeRenderer = (code: string, language: string) => ReactNode

class Walker {
  constructor(readonly math: string[], readonly display: Set<number>, readonly code?: CodeRenderer) {}

  /** Puts the math placeholders of a text run back as math. */
  text(s: string, key: string): ReactNode[] {
    if (!s.includes(OPEN)) return [s]
    const out: ReactNode[] = []
    let last = 0
    for (const m of s.matchAll(PLACEHOLDER)) {
      if (m.index! > last) out.push(s.slice(last, m.index))
      const tex = this.math[Number(m[1])]
      if (tex !== undefined) out.push(<InlineMath key={`${key}m${m.index}`} tex={tex} />)
      last = m.index! + m[0].length
    }
    if (last < s.length) out.push(s.slice(last))
    return out
  }

  inlines(tokens: Token[] | undefined, key: string): ReactNode[] {
    const out: ReactNode[] = []
    ;(tokens ?? []).forEach((t, n) => {
      const k = `${key}.${n}`
      switch (t.type) {
        case 'text': {
          const x = t as Tokens.Text
          out.push(...(x.tokens ? this.inlines(x.tokens, k) : this.text(x.text, k)))
          break
        }
        case 'escape': out.push((t as Tokens.Escape).text); break
        case 'strong': out.push(<strong key={k}>{this.inlines((t as Tokens.Strong).tokens, k)}</strong>); break
        case 'em': out.push(<em key={k}>{this.inlines((t as Tokens.Em).tokens, k)}</em>); break
        case 'del': out.push(<s key={k}>{this.inlines((t as Tokens.Del).tokens, k)}</s>); break
        case 'codespan': out.push(<code key={k}>{(t as Tokens.Codespan).text}</code>); break
        case 'br': out.push(<br key={k} />); break
        case 'link': {
          const l = t as Tokens.Link
          const href = safeHref(l.href)
          out.push(href ? <a key={k} href={href} target="_blank" rel="noreferrer noopener">{this.inlines(l.tokens, k)}</a> : <span key={k}>{this.inlines(l.tokens, k)}</span>)
          break
        }
        case 'image': out.push((t as Tokens.Image).text); break
        case 'html': out.push((t as Tokens.HTML).text); break
        case 'checkbox': break
        default: {
          const x = t as { tokens?: Token[]; text?: string }
          if (x.tokens) out.push(...this.inlines(x.tokens, k))
          else if (x.text) out.push(...this.text(x.text, k))
        }
      }
    })
    return out
  }

  /** A paragraph that is only a `$$…$$` placeholder. */
  private displayed(tokens: Token[] | undefined, raw: string): string | undefined {
    if (tokens && (tokens.length !== 1 || tokens[0].type !== 'text')) return undefined
    const m = /^(\d+)$/.exec(raw.trim())
    return m && this.display.has(Number(m[1])) ? this.math[Number(m[1])] : undefined
  }

  blocks(tokens: Token[], key: string): ReactNode[] {
    const out: ReactNode[] = []
    tokens.forEach((t, n) => {
      const b = this.block(t, `${key}.${n}`)
      if (b !== null) out.push(b)
    })
    return out
  }

  block(t: Token, k: string): ReactNode {
    switch (t.type) {
      case 'space': return null
      case 'paragraph': case 'text': {
        const p = t as Tokens.Paragraph | Tokens.Text
        const tex = this.displayed(p.tokens, p.text)
        if (tex !== undefined) return <MathBlock key={k} tex={tex} />
        return <p key={k}>{p.tokens ? this.inlines(p.tokens, k) : this.text(p.text, k)}</p>
      }
      case 'heading': {
        const h = t as Tokens.Heading
        const Tag = `h${Math.min(6, Math.max(1, h.depth))}` as 'h1'
        return <Tag key={k} className="md-heading">{this.inlines(h.tokens, k)}</Tag>
      }
      case 'code': {
        const c = t as Tokens.Code
        const lang = (c.lang ?? '').split(/\s/)[0]
        if (this.code) return <div key={k}>{this.code(c.text, lang)}</div>
        return <pre key={k} className="md-code">{c.text}</pre>
      }
      case 'blockquote': return <blockquote key={k}>{this.blocks((t as Tokens.Blockquote).tokens, k)}</blockquote>
      case 'list': {
        const l = t as Tokens.List
        const start = typeof l.start === 'number' ? l.start : 1
        return (
          <div key={k} className="md-list">
            {l.items.map((item, i) => (
              <div key={i} className="md-item">
                <span className="md-marker">
                  {item.task ? <span className={`md-check${item.checked ? ' on' : ''}`} aria-label={item.checked ? '完了' : '未完了'} /> : l.ordered ? `${start + i}.` : '•'}
                </span>
                <div className="md-item-body">{this.blocks(item.tokens, `${k}.${i}`)}</div>
              </div>
            ))}
          </div>
        )
      }
      case 'table': {
        const tb = t as Tokens.Table
        return (
          <div key={k} className="md-table-wrap">
            <table className="md-table">
              <thead><tr>{tb.header.map((c, i) => <th key={i}>{this.inlines(c.tokens, `${k}h${i}`)}</th>)}</tr></thead>
              <tbody>{tb.rows.map((r, i) => <tr key={i}>{r.map((c, j) => <td key={j}>{this.inlines(c.tokens, `${k}r${i}.${j}`)}</td>)}</tr>)}</tbody>
            </table>
          </div>
        )
      }
      case 'hr': return <hr key={k} />
      case 'html': {
        const h = t as Tokens.HTML
        if (this.code) return <div key={k}>{this.code(h.text, 'html')}</div>
        return <pre key={k} className="md-code">{h.text}</pre>
      }
      default: {
        const x = t as { tokens?: Token[]; text?: string }
        const inl = x.tokens ? this.inlines(x.tokens, k) : x.text ? this.text(x.text, k) : []
        return inl.length ? <p key={k}>{inl}</p> : null
      }
    }
  }
}

/** Markdown with math. `renderCode` draws code blocks (the chat's, with the
 *  language and a copy button); `fadeIn` fades new blocks in (an answer
 *  being written). */
export function Markdown({ text, className, renderCode, fadeIn }: { text: string; className?: string; renderCode?: CodeRenderer; fadeIn?: boolean }) {
  const doc = parse(text)
  const blocks = new Walker(doc.math, doc.displayed, renderCode).blocks(doc.tokens, 'b')
  return <div className={`markdown${fadeIn ? ' md-fade' : ''} ${className ?? ''}`}>{blocks}</div>
}
