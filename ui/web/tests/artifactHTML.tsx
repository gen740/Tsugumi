import assert from 'node:assert/strict'
import { parse, type DefaultTreeAdapterTypes as Tree } from 'parse5'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { ARTIFACT_HTML_LIMIT, ARTIFACT_HTML_POLICY, artifactHTML, staticArtifactHTML } from '../src/model/artifactHTML'
import { ArtifactHTMLPreview } from '../src/ui/artifactHTMLPreview'

// An HTML artifact can come from any Card, including one the Agent wrote. Every address
// below would reach this host if the preview ran, loaded or followed it.
const OUT = 'outside.invalid'
const PIXEL = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgaGj4DwADhAIAV8n6LgAAAABJRU5ErkJggg=='
const hostile = `<!DOCTYPE html>
<!-- a saved report -->
<html manifest="https://${OUT}/manifest">
<head>
<meta charset="utf-8">
<title>Report</title>
<base href="https://${OUT}/">
<meta http-equiv="refresh" content="0; url=https://${OUT}/refresh">
<meta http-equiv="Content-Security-Policy" content="default-src *">
<link rel="stylesheet" href="https://${OUT}/sheet.css">
<link rel="preconnect" href="https://${OUT}">
<link rel="stylesheet" href="data:text/css,p{color:teal}">
<script>document.documentElement.dataset.ran = 'inline'</script>
<script src="https://${OUT}/external.js"></script>
<style>@import url("https://${OUT}/import.css"); body { background: url(https://${OUT}/bg.png) }</style>
</head>
<body background="https://${OUT}/body.png" onload="document.documentElement.dataset.ran = 'onload'">
<h1 id="top">Result 21</h1>
<details open><summary>Source</summary><pre>saved input</pre></details>
<p style="background: url(https://${OUT}/inline.png)">kept text</p>
<img id="embedded" src="${PIXEL}" alt="figure">
<img src="https://${OUT}/pixel.png" srcset="https://${OUT}/2x.png 2x" onerror="document.documentElement.dataset.ran = 'onerror'">
<video poster="https://${OUT}/poster.png"><source src="https://${OUT}/movie.mp4"></video>
<a id="inside" href="#top">top</a>
<a id="out" href="https://${OUT}/link" ping="https://${OUT}/ping">out</a>
<a id="js" href="javascript:document.documentElement.dataset.ran='href'">js</a>
<a id="data-doc" href="data:text/html,%3Cimg%20src%3Dhttps%3A%2F%2Foutside.invalid%2Fnew%3E">data document</a>
<map><area id="data-area" href="data:text/html,another"></map>
<a id="spaced" href="  JavaScript:alert(1)">js</a>
<form action="https://${OUT}/submit" method="post"><input name="q" value="secret"><button formaction="https://${OUT}/button">go</button></form>
<iframe src="https://${OUT}/frame"></iframe>
<iframe srcdoc="<script>parent.document.documentElement.dataset.ran='srcdoc'</script>"></iframe>
<object data="https://${OUT}/object"></object>
<embed src="https://${OUT}/embed">
<noscript><img src="https://${OUT}/noscript.png"><script>document.documentElement.dataset.ran = 'noscript'</script></noscript>
<template><script>document.documentElement.dataset.ran = 'template'</script><img src="https://${OUT}/template.png"></template>
<svg xmlns:xlink="http://www.w3.org/1999/xlink" onload="document.documentElement.dataset.ran = 'svg'">
  <script>document.documentElement.dataset.ran = 'svgscript'</script>
  <script xlink:href="https://${OUT}/svg.js"></script>
  <image xlink:href="https://${OUT}/svg.png"></image>
  <use href="https://${OUT}/sprite.svg#a"></use>
  <a id="animated" href="#top"><set attributeName="href" to="javascript:document.documentElement.dataset.ran='smil'"></set><animate attributeName="xlink:href" values="https://${OUT}/animate"></animate><text>x</text></a>
  <circle r="4"><animate attributeName="r" values="4;8"></animate></circle>
</svg>
<math><mtext><script>document.documentElement.dataset.ran = 'math'</script></mtext></math>
</body>
</html>`

type Found = { tag: string; attrs: Map<string, string>; text: string }
function elements(html: string): Found[] {
  const found: Found[] = []
  const walk = (parent: Tree.ParentNode) => {
    for (const n of parent.childNodes) {
      if (!('tagName' in n)) continue
      const text = n.childNodes.map(c => ('value' in c ? c.value : '')).join('')
      found.push({ tag: n.tagName.toLowerCase(), attrs: new Map(n.attrs.map(a => [(a.prefix ? `${a.prefix}:${a.name}` : a.name).toLowerCase(), a.value])), text })
      if ('content' in n) walk(n.content)
      walk(n)
    }
  }
  // Read it back as the sandboxed frame does, with scripting off.
  walk(parse(html, { scriptingEnabled: false }))
  return found
}

const shown = staticArtifactHTML(hostile)
const all = elements(shown)
const tags = new Set(all.map(e => e.tag))

// Nothing that runs code, embeds another document or redirects this one is left, wherever
// it was: head, body, noscript, template, SVG or MathML.
for (const tag of ['script', 'base', 'iframe', 'object', 'embed', 'set']) assert.ok(!tags.has(tag), `${tag} was kept`)
for (const e of all) for (const name of e.attrs.keys()) assert.ok(!name.startsWith('on'), `${e.tag} kept ${name}`)

// The only policy is the preview's, and it comes before anything that could load.
const head = all.findIndex(e => e.tag === 'head')
assert.equal(all[head + 1].tag, 'meta')
assert.equal(all[head + 1].attrs.get('http-equiv'), 'Content-Security-Policy')
assert.equal(all[head + 1].attrs.get('content'), ARTIFACT_HTML_POLICY)
assert.deepEqual(all.filter(e => e.attrs.has('http-equiv')).length, 1)
assert.ok(ARTIFACT_HTML_POLICY.startsWith("default-src 'none'"))
for (const source of ARTIFACT_HTML_POLICY.split(';').flatMap(d => d.trim().split(/\s+/).slice(1))) {
  assert.ok(["'none'", "'unsafe-inline'", 'data:'].includes(source), `the policy allows ${source}`)
}
assert.ok(!/script-src|connect-src|frame-src|\*/.test(ARTIFACT_HTML_POLICY))

// No attribute still holds an address outside the document. Style text is the one place
// the address remains, and there the policy's `default-src 'none'` and `style-src` decide.
for (const e of all) for (const [name, value] of e.attrs) {
  if (name === 'style') continue
  assert.ok(!value.includes(OUT), `${e.tag} kept ${name}="${value}"`)
  assert.ok(!/^\s*javascript:/i.test(value), `${e.tag} kept ${name}="${value}"`)
}
assert.deepEqual(all.filter(e => e.tag === 'link').map(e => e.attrs.get('href')), ['data:text/css,p{color:teal}'])
assert.ok(all.filter(e => e.tag === 'animate').every(e => e.attrs.get('attributename') === 'r'), 'an animation could still write a link address')

// The document itself stays readable: text, the embedded image and links inside it.
assert.ok(shown.includes('Result 21') && shown.includes('kept text'))
assert.ok(all.some(e => e.tag === 'details' && e.attrs.has('open')))
assert.equal(all.find(e => e.attrs.get('id') === 'embedded')?.attrs.get('src'), PIXEL)
assert.equal(all.find(e => e.attrs.get('id') === 'inside')?.attrs.get('href'), '#top')
for (const id of ['out', 'js', 'spaced', 'data-doc', 'data-area']) assert.equal(all.find(e => e.attrs.get('id') === id)?.attrs.has('href'), false, id)
assert.ok(all.some(e => e.tag === 'form' && !e.attrs.has('action')))

// The doctype comes first, so the document is not shown in quirks mode.
assert.match(shown, /^<!DOCTYPE html>/)
assert.equal(parse(shown).mode, 'no-quirks')
assert.equal(parse(staticArtifactHTML('<p>no doctype</p>')).mode, 'quirks', 'a document without a doctype keeps its own mode')
const transitional = '<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">'
assert.ok(staticArtifactHTML(`${transitional}<p>x</p>`).startsWith(transitional))
assert.equal(parse(staticArtifactHTML(`${transitional}<p>x</p>`)).mode, parse(transitional).mode, 'an older doctype keeps its mode too')

// Cleaning what was shown changes nothing: the frame parses the same tree that was checked.
assert.equal(staticArtifactHTML(shown), shown)
// Text that only looks like markup after a second parse stays text.
const nested = elements(staticArtifactHTML('<!doctype html><textarea></textarea><script>1</script><p title="</p><script>2</script>">&lt;script&gt;3&lt;/script&gt;</p>'))
assert.ok(!nested.some(e => e.tag === 'script'))

// The frame gives the document no permission at all, and nothing but the static text.
const frame = renderToStaticMarkup(<ArtifactHTMLPreview html={shown} />)
const [iframe] = elements(frame).filter(e => e.tag === 'iframe')
assert.equal(iframe.attrs.get('sandbox'), '')
assert.equal(iframe.attrs.get('srcdoc'), shown)
assert.equal(iframe.attrs.has('src'), false)
assert.equal(iframe.attrs.get('referrerpolicy'), 'no-referrer')

// Which members get this preview: HTML by name, UTF-8, within the size limit.
const bytes = (text: string) => new TextEncoder().encode(text)
assert.equal(ARTIFACT_HTML_LIMIT, 32 * 1024 * 1024)
assert.ok(artifactHTML('report.html', bytes(hostile))?.startsWith('<!DOCTYPE html>'))
assert.ok(artifactHTML('nested/INDEX.HTM', bytes('<p>x</p>'))?.includes('<p>x</p>'))
for (const member of ['plot.png', 'data.csv', 'notes.html.txt', 'html', 'report.xhtml']) {
  assert.equal(artifactHTML(member, bytes('<p>x</p>')), undefined, member)
}
assert.equal(artifactHTML('report.html', new Uint8Array([0x3c, 0x70, 0x3e, 0xff, 0xfe])), undefined, 'not UTF-8')
const page = bytes('<!doctype html><p>size</p>')
assert.ok(artifactHTML('report.html', page, page.length), 'at the limit')
assert.equal(artifactHTML('report.html', page, page.length - 1), undefined, 'one byte over the limit')
assert.equal(artifactHTML('report.html', new Uint8Array(ARTIFACT_HTML_LIMIT + 1)), undefined, 'over 32 MiB')

console.log('HTML artifact preview: passed')
