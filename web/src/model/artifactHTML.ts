// The static form of an HTML artifact member. Any Card can save one, including a Card the
// Agent wrote, so the document is untrusted: the preview shows it without running its
// scripts, loading anything from outside the document, or following its links and forms.
import { parse, serialize, type DefaultTreeAdapterTypes as Tree } from 'parse5'

/** Larger HTML members are not previewed; they stay exportable. */
export const ARTIFACT_HTML_LIMIT = 32 * 1024 * 1024

/** Enforced by the browser for everything after it, so it is the first element of the head. */
export const ARTIFACT_HTML_POLICY =
  "default-src 'none'; img-src data:; style-src 'unsafe-inline' data:; font-src data:; base-uri 'none'; form-action 'none'"

// Elements that run code, load or embed another document, or redirect this one.
const REMOVED = new Set(['script', 'base', 'iframe', 'frame', 'frameset', 'object', 'embed', 'applet', 'portal', 'fencedframe'])
// Attributes holding an address the browser would load, submit to or navigate to.
const ADDRESSES = new Set([
  'href', 'xlink:href', 'src', 'srcset', 'imagesrcset', 'poster', 'data', 'action', 'formaction',
  'background', 'ping', 'manifest', 'codebase', 'longdesc', 'cite', 'usemap',
])

function attr(e: Tree.Element, name: string): string {
  return e.attrs.find(a => attrName(a) === name)?.value ?? ''
}

function attrName(a: { name: string; prefix?: string }): string {
  return (a.prefix ? `${a.prefix}:${a.name}` : a.name).toLowerCase()
}

/** Embedded data and places inside the document are the only addresses kept. */
function local(element: Tree.Element, value: string): boolean {
  const v = value.trim().toLowerCase()
  if (v.startsWith('#')) return true
  // A data: link could replace the sandboxed document with a new document
  // that has no preview policy. Embedded resources can still use data URLs.
  const tag = element.tagName.toLowerCase()
  return tag !== 'a' && tag !== 'area' && v.startsWith('data:')
}

function removed(e: Tree.Element): boolean {
  const tag = e.tagName.toLowerCase()
  if (REMOVED.has(tag)) return true
  if (tag === 'meta') return e.attrs.some(a => attrName(a) === 'http-equiv')
  // An embedded stylesheet is the one thing a link may bring in.
  if (tag === 'link') return !attr(e, 'href').trim().toLowerCase().startsWith('data:text/css')
  // SVG animation can write an address into a link after the attributes were checked.
  if (tag === 'set' || tag.startsWith('animate')) return attr(e, 'attributename').toLowerCase().includes('href')
  return false
}

function clean(parent: Tree.ParentNode) {
  parent.childNodes = parent.childNodes.filter(n => !('tagName' in n) || !removed(n))
  for (const n of parent.childNodes) {
    if (!('tagName' in n)) continue
    n.attrs = n.attrs.filter(a => {
      const name = attrName(a)
      return !name.startsWith('on') && !(ADDRESSES.has(name) && !local(n, a.value))
    })
    if ('content' in n) clean(n.content)
    clean(n)
  }
}

/**
 * The document as the preview frame receives it. It is parsed the way the frame parses it
 * (scripting off), so the tree that is cleaned is the tree that is shown, and the doctype
 * is kept so the document stays in its own rendering mode.
 */
export function staticArtifactHTML(html: string): string {
  const document = parse(html, { scriptingEnabled: false })
  clean(document)
  const root = document.childNodes.find((n): n is Tree.Element => 'tagName' in n && n.tagName === 'html')
  const head = root?.childNodes.find((n): n is Tree.Element => 'tagName' in n && n.tagName === 'head')
  if (!head) throw new Error('The HTML parser returned a document without a head')
  const policy = parse(`<meta http-equiv="Content-Security-Policy" content="${ARTIFACT_HTML_POLICY}">`)
  const meta = ((policy.childNodes[0] as Tree.Element).childNodes[0] as Tree.Element).childNodes[0] as Tree.Element
  meta.parentNode = head
  head.childNodes.unshift(meta)
  // The serializer writes only the doctype's name; its identifiers choose the rendering mode.
  const doctype = document.childNodes.find((n): n is Tree.DocumentType => n.nodeName === '#documentType')
  document.childNodes = document.childNodes.filter(n => n !== doctype)
  return (doctype ? doctypeText(doctype) : '') + serialize(document, { scriptingEnabled: false })
}

function doctypeText(d: Tree.DocumentType): string {
  const quoted = (id: string) => (id.includes('"') ? `'${id}'` : `"${id}"`)
  const ids = d.publicId ? ` PUBLIC ${quoted(d.publicId)}${d.systemId ? ` ${quoted(d.systemId)}` : ''}` : d.systemId ? ` SYSTEM ${quoted(d.systemId)}` : ''
  return `<!DOCTYPE ${d.name}${ids}>`
}

/**
 * The static document of an artifact member, when the member is HTML that can be previewed.
 * Anything else (another type, too large, not UTF-8) is left to the other previews.
 */
export function artifactHTML(member: string, data: Uint8Array, limit = ARTIFACT_HTML_LIMIT): string | undefined {
  if (!/\.html?$/i.test(member) || data.length > limit) return undefined
  let text: string
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(data)
  } catch {
    return undefined
  }
  return staticArtifactHTML(text)
}
