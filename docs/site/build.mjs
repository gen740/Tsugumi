// Writes the documentation site from the repository's Markdown: docs/index.html
// and docs/pages/*.html, for GitHub Pages serving docs/. The Markdown files stay
// the single source. Four pages ("Tsugumi を知る") get figures set in after
// phrases of their text; a phrase or section that is gone fails the build.
// For agents, each page also has its Markdown next to it (pages/x.md, with links
// that work on the site) and docs/llms.txt lists them (https://llmstxt.org/).
// Run after changing a document (tests/python/test_docs.py checks that it was):
//   node docs/site/build.mjs [--check]
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { Marked } from '../../web/node_modules/marked/lib/marked.esm.js'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..')
const REPO = 'https://github.com/gen740/Tsugumi'
const SITE = 'https://gen740.github.io/Tsugumi/'
const RAW = 'https://raw.githubusercontent.com/gen740/Tsugumi/main'
/** Documents an agent can skip when it needs less context ("Optional" in llms.txt). */
const SECONDARY = ['docs/Name.md', 'docs/Author.md', 'docs/Lock.md', 'docs/LicenseAudit.md', 'docs/ui-view.md']

/** Pages read with the thread and the moving figures. */
const STORY = [
  {
    out: 'index.html', source: 'README.md', title: 'Tsugumi が解決しようとしている課題', hero: true,
    sections: ['AI を研究に積極的に活用できていますか？', '処理の流れそのものを、人と AI の共通言語にする', '処理の形を描く。AI と実装する。実行して確かめる。'],
    figures: [
      ['これらに共通しているのは', 'wall', 'AI の出力が積み上がるほど、人と AI の間に壁ができていく。'],
      ['この三つを同じ Workflow', 'thread', 'Tsugumi は、その壁に一本の糸を通す。'],
      ['組み立てる macOS アプリ', 'sketch', 'もし、書いた通りのものが、そのまま形になったら？　紙に描いた三つの箱が、そのまま Draft になる。'],
      ['**確かめる。** Nix', 'run', 'Agent が Draft を実装し、実行すると、帯域ごとの光子数と RGB 画像がそのまま Canvas に並ぶ（同梱の Cas A · Chandra RGB デモ）。'],
    ],
  },
  { out: 'pages/philosophy.html', source: 'docs/Philosophy.md' },
  {
    out: 'pages/name.html', source: 'docs/Name.md',
    figures: [['Card と Card をつなぐ配線', 'name', '手毬から引き出した一本の糸が、Card と Card をつなぐ配線になる。']],
  },
  {
    out: 'pages/author.html', source: 'docs/Author.md',
    figures: [['AI for astrophysics', 'author', '東京大学・安田講堂。星をつなぐように、AI で宇宙を読み解く研究をしています。']],
  },
]

const read = (rel) => fs.readFileSync(path.join(ROOT, rel), 'utf8')
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')

/** The same anchors as GitHub's (and tests/python/test_docs.py). */
function slug(heading) {
  return heading.replace(/\[([^\]]*)\]\([^)]*\)/g, '$1').replace(/[`*]/g, '').trim().toLowerCase()
    .replace(/[^\p{L}\p{N}_\- 　]/gu, '').replace(/ /g, '-')
}

const titleOf = (rel) => /^# (.+)$/m.exec(read(rel))?.[1].replace(/[`*]/g, '') ?? rel

/** Every other document: docs/README.md is the list, grouped as it groups them. */
const notes = new Map()
function documentGroups() {
  const groups = []
  let group = null
  for (const line of read('docs/README.md').split('\n')) {
    const h = /^## (.+)$/.exec(line)
    if (h) { group = { title: h[1], docs: [] }; groups.push(group); continue }
    const row = /^\| \[[^\]]+\]\(([^)]+)\) \|/.exec(line)
    if (row && group) group.docs.push(path.normalize(path.join('docs', row[1])))
    const note = /^\| \[[^\]]+\]\(([^)]+)\) \| (.+) \|$/.exec(line)
    if (note) notes.set(path.normalize(path.join('docs', note[1])), note[2])
  }
  return groups
}

const outName = (rel) => {
  if (rel === 'README.md') return 'pages/readme.html'
  if (rel === 'docs/README.md') return 'pages/documents.html'
  return `pages/${path.basename(rel, '.md').toLowerCase().replace(/_/g, '-')}.html`
}

function pageMap(groups) {
  const map = new Map()
  for (const s of STORY) map.set(s.source, s.out)
  map.set('README.md', 'pages/readme.html')  // the whole README; index.html is its story
  map.set('docs/README.md', 'pages/documents.html')
  for (const g of groups) for (const d of g.docs) if (!map.has(d)) map.set(d, outName(d))
  map.set('THIRD_PARTY_NOTICES.md', outName('THIRD_PARTY_NOTICES.md'))
  return map
}

/** Links of a document as links of the site; everything else in the repository on GitHub. */
function linker(sourceRel, outRel, pages) {
  const up = (to) => path.relative(path.dirname(outRel), to).split(path.sep).join('/') || path.basename(to)
  return (href) => {
    if (!href || /^[a-z][a-z0-9+.-]*:/i.test(href) || href.startsWith('#')) return href
    const [target, anchor] = href.split('#')
    const rel = path.normalize(path.join(path.dirname(sourceRel), target))
    const tail = anchor ? `#${anchor}` : ''
    if (pages.has(rel)) {
      const to = pages.get(rel)
      return (to === outRel ? '' : up(to)) + tail || tail
    }
    if (rel.startsWith('docs/assets/') || rel.startsWith('docs/site/') || rel === 'docs/llms.txt') return up(rel.slice('docs/'.length))
    const abs = path.join(ROOT, rel)
    const kind = fs.existsSync(abs) && fs.statSync(abs).isDirectory() ? 'tree' : 'blob'
    return `${REPO}/${kind}/main/${rel}${tail}`
  }
}

function makeMarked(link) {
  const seen = new Map()
  const m = new Marked({ gfm: true })
  m.use({
    renderer: {
      heading({ tokens, depth, raw }) {
        const text = raw.replace(/^#+\s*/, '').replace(/\s*$/, '')
        let id = slug(text)
        const n = seen.get(id) ?? 0
        seen.set(id, n + 1)
        if (n) id += `-${n}`
        return `<h${depth} id="${esc(id)}">${this.parser.parseInline(tokens)}</h${depth}>\n`
      },
      // Japanese lines of a paragraph join without the space a line break would show.
      paragraph({ tokens }) {
        return `<p>${this.parser.parseInline(tokens).replace(/([^\x00-\x7f])\n(?=[^\x00-\x7f])/g, '$1')}</p>\n`
      },
      link({ href, title, tokens }) {
        const to = link(href)
        const external = /^https?:/.test(to)
        return `<a href="${esc(to)}"${title ? ` title="${esc(title)}"` : ''}${external ? ' rel="noopener"' : ''}>${this.parser.parseInline(tokens)}</a>`
      },
      image({ href, title, text }) {
        return `<img src="${esc(link(href))}" alt="${esc(text)}"${title ? ` title="${esc(title)}"` : ''} loading="lazy">`
      },
      html({ text }) {
        return text.replace(/<!--[\s\S]*?-->/g, '').replace(/\b(src|href)="([^"]+)"/g, (_, a, v) => `${a}="${esc(link(v))}"`)
      },
      table(token) {
        const cell = (c) => `<${c.header ? 'th' : 'td'}${c.align ? ` style="text-align:${c.align}"` : ''}>${this.parser.parseInline(c.tokens)}</${c.header ? 'th' : 'td'}>`
        const row = (cells) => `<tr>${cells.map(cell).join('')}</tr>`
        return `<div class="table-wrap"><table><thead>${row(token.header)}</thead><tbody>${token.rows.map(row).join('')}</tbody></table></div>\n`
      },
    },
  })
  return m
}

/** The blocks of a story page: sections, without the way back to the README, with figures. */
function storyBody(story, marked) {
  let tokens = marked.lexer(read(story.source))
  if (story.sections) {
    const picked = []
    for (const title of story.sections) {
      const start = tokens.findIndex((t) => t.type === 'heading' && t.depth === 2 && t.text === title)
      if (start < 0) throw new Error(`${story.source}: no section "${title}"`)
      let end = start + 1
      while (end < tokens.length && !(tokens[end].type === 'heading' && tokens[end].depth === 2)) end++
      picked.push(...tokens.slice(start, end))
    }
    tokens = picked
    tokens.unshift({ type: 'heading', depth: 1, raw: `# ${story.title}`, text: story.title, tokens: marked.Lexer.lexInline(story.title) })
  }
  tokens = tokens.filter((t) => !(t.type === 'paragraph' && t.raw.startsWith('[README に戻る]')) && t.type !== 'space')
  const after = new Map()
  for (const [phrase, id, caption] of story.figures ?? []) {
    const i = tokens.findIndex((t) => t.raw.includes(phrase))
    if (i < 0) throw new Error(`${story.source}: no block has "${phrase}" (figure ${id})`)
    after.set(i, { id, caption })
  }
  let html = ''
  tokens.forEach((t, i) => {
    html += marked.parser([t])
    const f = after.get(i)
    if (f) html += `<figure class="ex-fig" data-fig="${f.id}"><div class="ex-fig-space"></div><figcaption>${esc(f.caption)}</figcaption></figure>\n`
  })
  return html
}

function sidebar(groups, pages, outRel) {
  const href = (to) => (to === outRel ? '#' : path.relative(path.dirname(outRel), to).split(path.sep).join('/'))
  const item = (to, title, sub) => `<li${to === outRel ? ' class="on" aria-current="page"' : ''}><a href="${href(to)}">${esc(title)}${sub ? `<small>${esc(sub)}</small>` : ''}</a></li>`
  let out = `<div class="nav-group"><h2>Tsugumi を知る</h2><ul>`
  for (const s of STORY) out += item(s.out, s.title ?? titleOf(s.source), s.source)
  out += '</ul></div>'
  out += `<div class="nav-group"><h2>全体</h2><ul>${item('pages/readme.html', 'README')}${item('pages/documents.html', 'ドキュメント一覧')}</ul></div>`
  for (const g of groups) {
    const docs = g.docs.filter((d) => !STORY.some((s) => s.source === d))
    out += `<div class="nav-group"><h2>${esc(g.title)}</h2><ul>${docs.map((d) => item(pages.get(d), titleOf(d))).join('')}</ul></div>`
  }
  return out
}

/** Where a document's Markdown is on the site: next to its page; the README at the top. */
const markdownOut = (source, pages) => (source === 'README.md' ? 'index.md' : pages.get(source).replace(/\.html$/, '.md'))

/** A document as it is, with links that work on the site (outside code blocks). */
function markdownPage(source, outRel, mdPages) {
  const link = linker(source, outRel, mdPages)
  return read(source).split(/(^```[\s\S]*?^```)/m).map((part, i) => (i % 2
    ? part
    : part.replace(/\]\(([^)\s]+)\)/g, (_, to) => `](${link(to)})`).replace(/\b(src|href)="([^"]+)"/g, (_, a, to) => `${a}="${link(to)}"`))).join('')
}

/** The guide for agents: https://llmstxt.org/ */
function llmsTxt(groups, mdPages, tagline) {
  const readme = read('README.md').split('\n')
  const start = readme.indexOf(`**${tagline}**`) + 2
  const about = readme.slice(start, readme.indexOf('', start)).join('\n')
  const item = (d) => `- [${titleOf(d)}](${SITE}${mdPages.get(d)})${notes.has(d) ? `: ${notes.get(d)}` : ''}`
  const out = [
    '# Tsugumi', '', `> ${tagline}`, '', about, '',
    '文書は人間と AI の両方に向けて書いています。下の URL は各ページの Markdown 版です。主張の確かめ方は「主張と、それを確かめる方法」に、コード・テスト・コマンドと一緒に並べています。', '',
    '## 全体', '', `- [README](${SITE}index.md): 課題意識、使い方、実画面の説明`, '',
  ]
  for (const g of groups) {
    const docs = g.docs.filter((d) => !SECONDARY.includes(d))
    out.push(`## ${g.title}`, '', ...docs.map(item), '')
  }
  out.push('## リポジトリ', '',
    `- [AGENTS.md](${RAW}/AGENTS.md): 地図、主張と確かめる場所、このリポジトリで作業するエージェントへの指示`,
    `- [Card の仕様](${RAW}/agent/Reference/card-guide.md): アプリの Agent が読むものと同じ文書`,
    `- [RPC の契約](${RAW}/protocol/tsugumi.fbs): UI・Web・Agent が使う契約の正本`, '')
  out.push('## Optional', '', ...SECONDARY.map(item), '')
  return out.join('\n')
}

function page({ outRel, title, source, markdown, description, body, nav, story, hero }) {
  const asset = (p) => path.relative(path.dirname(outRel), p).split(path.sep).join('/')
  const canonical = SITE + (outRel === 'index.html' ? '' : outRel)
  return `<!doctype html>
<!-- Generated by docs/site/build.mjs from ${source}: do not edit. -->
<html lang="ja"${story ? ' class="story"' : ''}>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)} · Tsugumi</title>
<meta name="description" content="${esc(description)}">
<link rel="canonical" href="${canonical}">
<link rel="alternate" type="text/markdown" href="${asset(markdown)}">
<link rel="describedby" href="${asset('llms.txt')}">
<link rel="icon" href="${asset('assets/tsugumi-icon.svg')}" type="image/svg+xml">
<link rel="stylesheet" href="${asset('site/site.css')}">
</head>
<body>
<div class="layout">
<nav class="side" aria-label="文書">
  <div class="brand"><a href="${asset('index.html')}"><img src="${asset('assets/tsugumi-icon.svg')}" alt="" width="28" height="28">Tsugumi</a><span class="brand-links"><a class="repo" href="${asset('llms.txt')}" title="AI 向けの案内">llms.txt</a><a class="repo" href="${REPO}" rel="noopener">GitHub</a></span></div>
  <details class="nav-fold" open><summary>文書の一覧</summary>${nav}</details>
  <script>{ const fold = document.currentScript.previousElementSibling, narrow = matchMedia('(max-width: 820px)'), set = () => { fold.open = !narrow.matches }; set(); narrow.addEventListener('change', set) }</script>
</nav>
<main class="main">
${hero ?? ''}${story ? '<div class="mode" role="group" aria-label="読み方"><button type="button" data-mode="read" aria-pressed="true">読む</button><button type="button" data-mode="cards" aria-pressed="false">カード</button></div>' : ''}
<p class="source"><a href="${REPO}/blob/main/${source}" rel="noopener"><code>${esc(source)}</code></a> と同じ文書です（<a href="${asset(markdown)}">Markdown</a>）</p>
<article class="doc">
${body}</article>
</main>
</div>
${story ? `<script type="module" src="${asset('site/js/reader.js')}"></script>` : ''}
</body>
</html>
`
}

function build() {
  const groups = documentGroups()
  const pages = pageMap(groups)
  const out = new Map()
  const readme = read('README.md')
  const tagline = /^\*\*(.+)\*\*$/m.exec(readme)?.[1] ?? 'Tsugumi'

  for (const s of STORY) {
    const marked = makeMarked(linker(s.source, s.out, pages))
    const hero = s.hero
      ? `<header class="hero"><img src="assets/tsugumi-hero.svg" alt="Tsugumi"><p>${esc(tagline)}</p><a class="cta" href="pages/readme.html#使ってみる">使ってみる</a></header>\n`
      : ''
    out.set(s.out, page({
      outRel: s.out, title: s.title ?? titleOf(s.source), source: s.source, markdown: markdownOut(s.source, pages), description: tagline,
      body: storyBody(s, marked), nav: sidebar(groups, pages, s.out), story: true, hero,
    }))
  }
  for (const [source, outRel] of pages) {
    if (out.has(outRel) || STORY.some((s) => s.out === outRel)) continue
    const marked = makeMarked(linker(source, outRel, pages))
    out.set(outRel, page({
      outRel, title: titleOf(source), source, markdown: markdownOut(source, pages), description: tagline,
      body: marked.parse(read(source)), nav: sidebar(groups, pages, outRel), story: false,
    }))
  }
  const mdPages = new Map([...pages.keys()].map((source) => [source, markdownOut(source, pages)]))
  for (const [source, rel] of mdPages) out.set(rel, markdownPage(source, rel, mdPages))
  out.set('llms.txt', llmsTxt(groups, mdPages, tagline))
  return out
}

const made = build()
const docs = path.join(ROOT, 'docs')
const existing = fs.existsSync(path.join(docs, 'pages'))
  ? fs.readdirSync(path.join(docs, 'pages')).map((f) => `pages/${f}`) : []

if (process.argv.includes('--check')) {
  const stale = [...made].filter(([rel, html]) => !fs.existsSync(path.join(docs, rel)) || fs.readFileSync(path.join(docs, rel), 'utf8') !== html).map(([rel]) => rel)
  const extra = existing.filter((rel) => !made.has(rel))
  if (stale.length || extra.length) {
    console.error('out of date:', ...stale, ...extra)
    process.exit(1)
  }
} else {
  fs.mkdirSync(path.join(docs, 'pages'), { recursive: true })
  for (const rel of existing) if (!made.has(rel)) fs.rmSync(path.join(docs, rel))
  for (const [rel, html] of made) fs.writeFileSync(path.join(docs, rel), html)
  fs.writeFileSync(path.join(docs, '.nojekyll'), '')
  console.log(`${made.size} files`)
}
