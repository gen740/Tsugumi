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

/** Pages read with the thread and the moving figures, in the order the slide show goes through them. */
const STORY = [
  {
    out: 'pages/problem.html', source: 'README.md', title: 'Tsugumi が解決しようとしている課題', know: true,
    // A section, or its beginning: upTo names the first block left out.
    sections: [
      { title: 'AI を研究に積極的に活用できていますか？' },
      { title: '処理の流れそのものを、人と AI の共通言語にする', upTo: 'この仕組みは、研究を出発点に' },
      { title: 'AI に頼みたいことは、本来単純なはずです' },
      { title: '処理の形を描く。AI と実装する。実行して確かめる。' },
    ],
    figures: [
      ['**AI に仕事を任せるには', 'summit', 'AI が登った道を、確かめられる足場として残す。だから人が登っていける。'],
      ['これらに共通しているのは', 'wall', 'AI の出力が積み上がるほど、人と AI の間に壁ができていく。'],
      ['この三つを同じ Workflow', 'thread', 'Tsugumi は、積み上がった壁を越えて、人と AI を一本の糸でつなぐ。'],
      ['紙に箱を三つ', 'sketch', '紙に描いた、三つの箱。'],
      ['組み立てる macOS アプリ', 'draft', 'もし、書いた通りのものが、そのまま形になったら？　三つの箱が、そのまま Draft になる。'],
      ['**確かめる。** Nix', 'run', 'Agent が Draft を実装し、実行すると、帯域ごとの光子数と RGB 画像がそのまま Canvas に並ぶ（同梱の Cas A · Chandra RGB デモ）。'],
      ['**確かめる。** Nix', { image: 'assets/screenshots/workflow.png', width: 1440, height: 900, alt: 'Cas A の X 線観測から RGB 画像とエネルギースペクトルを作る Canvas' }, '実際の画面。深い観測で実行すると、スペクトルの輝線と画像の色の対応まで同じ Canvas で確かめられる。'],
    ],
  },
  {
    out: 'pages/philosophy.html', source: 'docs/Philosophy.md', know: true,
    figures: [
      ['AI を動かす速度や手軽さは', 'outpace', 'AI が試す速さと、人が理解する速さ。その差は開いていく。'],
      ['しばらく運転から離れていた人間が', 'driver', '任せていた人が、ある瞬間に引き継ぎを求められる。'],
      ['もし、相関が強く見える', 'curve', '選ばれた点だけの美しい線と、除かれていた点も入れた線。'],
    ],
  },
  {
    out: 'pages/name.html', source: 'docs/Name.md', know: true,
    figures: [
      ['Card と Card をつなぐ配線', 'name', '手毬から引き出した一本の糸が、Card と Card をつなぐ配線になる。'],
      ['ツグミは、冬鳥として', 'walk', '数歩跳ねては立ち止まり、胸を張ってあたりを見渡す。'],
      ['言葉を重ねて結果を語る時代に', 'hush', '多くを語らず、試行の積み重ねを差し出す。'],
    ],
  },
  {
    out: 'pages/author.html', source: 'docs/Author.md', know: true,
    figures: [['AI for astrophysics', 'author', '東京大学・安田講堂。星をつなぐように、AI で宇宙を読み解く研究をしています。']],
  },
  // The author's own words: figures without a caption and without words of their own, and its short paragraphs gathered on a slide.
  {
    out: 'pages/origin.html', source: 'docs/Origin.md', know: true, gather: true,
    figures: [
      ['人間にしかできないことって何でしょうか', 'asking'],
      ['宇宙環境特有のノイズを含んでいたり', 'orbit'],
      ['宇宙は全人類のものです', 'sky'],
      ['全てのデータはインターネットで公開され', 'reach'],
      ['けれども、ずっと あと一歩が足りない', 'trials'],
      ['Claude はその想いを形にしてくれました', 'formed'],
      ['最後に全ての同志に呼びかけたい', 'fellows'],
    ],
  },
  {
    out: 'pages/qa.html', source: 'docs/QA.md',
    figures: [
      ['AI に解析を任せると', 'oneGraph', '人が描くもの、AI が編集するもの、実際に動くものが、一つに重なる。'],
      ['Tsugumi では、保存された最新入力を', 'partial', '直した Card から、保存された入力で試す。最後は全体を通す。'],
    ],
  },
  {
    out: 'pages/novelty.html', source: 'docs/novelty.md',
    figures: [
      ['Tsugumi は、コードや Workflow の定義を Git で管理し', 'branches', '一つの結果から、別の仮説へ。どの枝も、たどって戻れる。'],
    ],
  },
  {
    out: 'pages/vision.html', source: 'docs/Vision.md',
    figures: [
      ['学ぶ側は入力やパラメータを', 'teach', '条件を自分で変えて、結論がどう動くかを確かめる。'],
      ['例えば毎月の集計業務を引き継ぐとき', 'handoff', '意図とコードと環境を一緒に渡し、次の人がそこから試し続ける。'],
    ],
  },
]

/** What the list leaves out: reached from Home (the download, the gallery) or from the list of documents. */
const OFF_THE_LIST = new Set(['INSTALL.md', 'docs/Demos.md', 'docs/Limitations.md'])

/** The demos of the gallery on Home (demos/index.json has the rest), in plain words. */
const GALLERY = [
  ['casa', 'Cas A · Chandra RGB', 'X 線観測を 3 つのエネルギー帯に分けて RGB 画像にし、同じ光子のスペクトルで輝線と色の対応を見る。'],
  ['report', 'データからレポートへ', 'データ生成・集計・レポート作成をつなぐ。途中を変えて、そこから再実行する。'],
  ['collatz', 'Collatz (C++ / Python)', '二つの言語で同じ計算をして、分岐と反復をたどりながら結果を比べる。'],
  ['plots', 'Plots', 'ウィンドウ向けに書いた matplotlib のコードが、そのまま Card の表示になる。'],
  ['live-stream', 'Live Stream', '時系列の出力を、実行中にそのままグラフで描く。'],
  ['gw150914', 'GW150914', '公開データから、重力波のチャープを取り出して図にする。'],
  ['protein-6lu7', '6LU7', 'タンパク質の立体構造を取得して、結合ポケットを見る。'],
  ['exoplanet-transit', 'Kepler-6 b', '光度曲線を折りたたんで、系外惑星のトランジットを見る。'],
  ['seismic-trench', '日本海溝', '地震カタログから、震源の断面と b 値を見る。'],
  ['dynamics-draft', '力学系ギャラリー', '6 つの Draft を、Agent が helper に手分けさせて同時に実装する。'],
]
const VERSION_NOW = () => read('VERSION').trim()
const DOWNLOAD = `${REPO}/releases/latest`

const ICON = {
  github: '<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27s1.36.09 2 .27c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8Z"/></svg>',
  panel: '<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.4"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><path d="M6 2.5v11"/></svg>',
  download: '<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M8 2v8M4.5 7 8 10.5 11.5 7M2.5 14h11"/></svg>',
}

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
  map.set('README.md', 'pages/readme.html')  // the whole README; pages/problem.html is its story
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
  const headings = []
  const m = new Marked({ gfm: true })
  m.headings = headings
  m.use({
    renderer: {
      heading({ tokens, depth, raw }) {
        const text = raw.replace(/^#+\s*/, '').replace(/\s*$/, '')
        let id = slug(text)
        const n = seen.get(id) ?? 0
        seen.set(id, n + 1)
        if (n) id += `-${n}`
        const inner = this.parser.parseInline(tokens)
        headings.push({ depth, id, html: inner.replace(/<\/?a\b[^>]*>/g, '') })
        return `<h${depth} id="${esc(id)}">${inner}</h${depth}>\n`
      },
      // Japanese lines of a paragraph join without the space a line break would show.
      paragraph({ tokens }) {
        return `<p>${this.parser.parseInline(tokens).replace(/([^\x00-\x7f])\n(?=[^\x00-\x7f])|([。！？」）])\n/g, '$1$2')}</p>\n`
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
      // A drawing in text lines up only if a Japanese character takes exactly two columns.
      code({ text, lang }) {
        const body = /[─│┌┐└┘├┤┬┴┼]/.test(text)
          ? esc(text).replace(/[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\u3000-\u303f\u30fb\u30fc\uff01-\uff60]/gu, '<span class="wide">$&</span>')
          : esc(text)
        return `<pre><code${lang ? ` class="language-${esc(lang)}"` : ''}>${body}\n</code></pre>\n`
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
    for (const { title, upTo } of story.sections) {
      const start = tokens.findIndex((t) => t.type === 'heading' && t.depth === 2 && t.text === title)
      if (start < 0) throw new Error(`${story.source}: no section "${title}"`)
      let end = start + 1
      while (end < tokens.length && !(tokens[end].type === 'heading' && tokens[end].depth === 2)) end++
      const part = tokens.slice(start, end)
      const cut = upTo ? part.findIndex((t) => t.raw.includes(upTo)) : part.length
      if (cut < 0) throw new Error(`${story.source}: section "${title}" has no block with "${upTo}"`)
      picked.push(...part.slice(0, cut))
    }
    tokens = picked
    tokens.unshift({ type: 'heading', depth: 1, raw: `# ${story.title}`, text: story.title, tokens: marked.Lexer.lexInline(story.title) })
  }
  tokens = tokens.filter((t) => !(t.type === 'paragraph' && t.raw.startsWith('[README に戻る]')) && t.type !== 'space')
  const after = new Map()
  for (const [phrase, what, caption] of story.figures ?? []) {
    const i = tokens.findIndex((t) => t.raw.includes(phrase))
    if (i < 0) throw new Error(`${story.source}: no block has "${phrase}" (figure ${what.image ?? what})`)
    after.set(i, [...(after.get(i) ?? []), { what, caption }])
  }
  let html = ''
  tokens.forEach((t, i) => {
    html += marked.parser([t])
    for (const { what, caption } of after.get(i) ?? []) {
      html += what.image
        ? `<figure class="shot"><img src="${esc(path.relative(path.dirname(story.out), what.image))}" alt="${esc(what.alt)}" width="${what.width}" height="${what.height}" loading="lazy"><figcaption>${esc(caption)}</figcaption></figure>\n`
        : `<figure class="ex-fig" data-fig="${what}"><div class="ex-fig-pin"><div class="ex-fig-space"></div>${caption ? `<figcaption>${esc(caption)}</figcaption>` : ''}</div></figure>\n`
    }
  })
  return html
}

/** The headings of a long page, as links for beside (or above) the text. */
function pageContents(headings) {
  const shown = headings.filter((h) => h.depth === 2 || h.depth === 3)
  if (shown.length < 5) return ''
  const items = shown.map((h) => `<li class="d${h.depth}"><a href="#${esc(h.id)}">${h.html}</a></li>`).join('')
  return `<details class="toc" open><summary>目次</summary><ul>${items}</ul></details>
<script>{ const fold = document.currentScript.previousElementSibling, narrow = matchMedia('(max-width: 1240px)'), set = () => { fold.open = !narrow.matches }; set(); narrow.addEventListener('change', set) }</script>
`
}

function sidebar(groups, pages, outRel) {
  const href = (to) => (to === outRel ? '#' : path.relative(path.dirname(outRel), to).split(path.sep).join('/'))
  const item = (to, title) => `<li${to === outRel ? ' class="on" aria-current="page"' : ''}><a href="${href(to)}">${esc(title)}</a></li>`
  let out = `<div class="nav-group"><h2>Tsugumi を知る</h2><ul>`
  for (const s of STORY.filter((x) => x.know)) out += item(s.out, s.title ?? titleOf(s.source))
  out += '</ul></div>'
  for (const g of groups) {
    const docs = g.docs.filter((d) => !OFF_THE_LIST.has(d) && !STORY.some((s) => s.know && s.source === d))
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

/** The lines under the README's tagline: what Tsugumi is. */
function about(readme, tagline) {
  const lines = readme.split('\n')
  const start = lines.indexOf(`**${tagline}**`) + 2
  return lines.slice(start, lines.indexOf('', start))
}

/** The guide for agents: https://llmstxt.org/ */
function llmsTxt(groups, mdPages, tagline) {
  const item = (d) => `- [${titleOf(d)}](${SITE}${mdPages.get(d)})${notes.has(d) ? `: ${notes.get(d)}` : ''}`
  const out = [
    '# Tsugumi', '', `> ${tagline}`, '', about(read('README.md'), tagline).join('\n'), '',
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

function page({ outRel, title, source, markdown, description, body, nav, story, home, whole, toc, neighbours, embeds, gather }) {
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
${neighbours?.next ? `<meta name="story-next" content="${asset(neighbours.next)}">\n` : ''}${neighbours?.prev ? `<meta name="story-prev" content="${asset(neighbours.prev)}">\n` : ''}<script>try { if (localStorage.getItem('tsugumi.side') === 'closed') document.documentElement.classList.add('side-closed') } catch { /* the page works without it */ }</script>
</head>
<body>
<button type="button" class="side-open" aria-label="サイドバーを開く" title="サイドバーを開く">${ICON.panel}</button>
<div class="layout">
<nav class="side" aria-label="文書">
  <div class="brand"><a href="${asset('index.html')}"><img src="${asset('assets/tsugumi-icon.svg')}" alt="" width="28" height="28">Tsugumi</a><span class="brand-links"><a class="repo" href="${REPO}" rel="noopener">${ICON.github}GitHub</a><button type="button" class="side-close" aria-label="サイドバーを閉じる" title="サイドバーを閉じる">${ICON.panel}</button></span></div>
  <details class="nav-fold" open><summary>文書の一覧</summary>${nav}</details>
  <script>{
    const fold = document.currentScript.previousElementSibling, narrow = matchMedia('(max-width: 820px)'), set = () => { fold.open = !narrow.matches }; set(); narrow.addEventListener('change', set)
    const side = fold.parentElement, on = fold.querySelector('li.on')
    if (on && on.offsetTop + on.offsetHeight > side.clientHeight) side.scrollTop = on.offsetTop - side.clientHeight / 2
    const root = document.documentElement, turn = (closed) => { root.classList.toggle('side-closed', closed); try { localStorage.setItem('tsugumi.side', closed ? 'closed' : 'open') } catch { /* the page works without it */ } }
    side.querySelector('.side-close').addEventListener('click', () => turn(true))
    document.querySelector('.side-open').addEventListener('click', () => turn(false))
  }</script>
</nav>
<main class="main${home ? ' main-home' : ''}">
${home ?? `${story ? '<div class="mode" role="group" aria-label="読み方"><button type="button" data-mode="read" aria-pressed="true">読む</button><button type="button" data-mode="cards" aria-pressed="false">スライドショー</button></div>' : ''}
<div class="reading">
${toc ?? ''}<article class="doc"${gather ? ' data-gather=""' : ''}>
${body}</article>
</div>
<p class="source">${whole
    ? `<a href="${REPO}/blob/main/${source}" rel="noopener"><code>${esc(source)}</code></a> の一部から作ったページです（<a href="${asset(whole)}">全文</a> · <a href="${asset(markdown)}">Markdown</a>）`
    : `<a href="${REPO}/blob/main/${source}" rel="noopener"><code>${esc(source)}</code></a> と同じ文書です（<a href="${asset(markdown)}">Markdown</a>）`}</p>
`}</main>
</div>
${home ? `<script type="module" src="${asset('site/js/home.js')}"></script>\n` : ''}${story ? `<script type="module" src="${asset('site/js/reader.js')}"></script>\n` : ''}${toc ? `<script src="${asset('site/js/toc.js')}" defer></script>\n` : ''}${embeds ? `<script type="module" src="${asset('site/js/embed.js')}"></script>\n` : ''}
</body>
</html>
`
}

/** Home: the thread from the temari runs through the ways in, as a Workflow of documents. */
function homePage(groups, pages, tagline, lead) {
  const here = (to) => to
  const doorText = { 'はじめる': '使い始める', '考え方を知る': 'なぜこの形なのか', '仕組みを知る': '中がどうなっているか', '記録と開発': '確かめられるように' }
  const know = STORY.filter((s) => s.know)
  const row = (to, title, note) => `<li><a href="${here(to)}"><b>${esc(title)}</b>${note ? `<span>${esc(note)}</span>` : ''}</a></li>`
  const shortNote = (d) => (notes.get(d) ?? '').replace(/^(「[^」]+」——|[^:：]{1,14}[:：]\s*)/, '').replace(/`/g, '').replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
  const door = (id, name, sub, items) => `<section class="door" data-door="${id}"><header><i>&lt;/&gt;</i><h2>${esc(name)}</h2><small>${esc(sub)}</small></header><ul>${items}</ul></section>`
  const doors = [
    door('know', 'Tsugumi を知る', 'どんな課題に、どう向き合っているか', know.map((s) => row(s.out, s.title ?? titleOf(s.source), s.sections ? '壁と糸、三つの箱から Cas A まで' : shortNote(s.source))).join('')),
    ...groups.map((g) => {
      const docs = g.docs.filter((d) => !OFF_THE_LIST.has(d) && !know.some((s) => s.source === d))
      return door(g.title, g.title, doorText[g.title] ?? '', docs.map((d) => row(pages.get(d), titleOf(d), shortNote(d))).join(''))
    }),
  ].join('')
  const tiles = GALLERY.map(([id, title, text]) => `<button type="button" class="demo-tile" data-demo="${id}"><span class="demo-thumb"></span><b>${esc(title)}</b><span>${esc(text)}</span></button>`).join('')
  return `<article class="home">
<header class="home-hero">
  <div class="home-copy">
    <h1>Tsugumi</h1>
    <p class="tagline">${esc(tagline)}</p>
    <p class="lead">${esc(lead)}</p>
    <div class="hero-actions">
      <a class="cta" href="${DOWNLOAD}">${ICON.download}Mac 版をダウンロード</a>
      <a class="cta quiet" href="#demos">インストールせずにデモを見る</a>
    </div>
    <p class="home-note">v${esc(VERSION_NOW())} · Apple silicon / macOS 26 以降 · <a href="${pages.get('INSTALL.md')}">インストールと開発用ビルド</a></p>
  </div>
  <div class="home-perch" aria-hidden="true"></div>
</header>
<div class="doors">${doors}</div>
<section class="gallery" id="demos">
  <h2>デモを、そのまま動かしてみる</h2>
  <p>同梱のデモです。インストールしなくても、Card のつながりと実行の様子をここで見られます。押すと大きく開きます。一つずつの説明は<a href="${pages.get('docs/Demos.md')}">同梱デモのしくみ</a>にあります。</p>
  <div class="demo-grid">${tiles}</div>
</section>
<p class="home-foot"><a href="${pages.get('README.md')}">README</a> · <a href="${pages.get('docs/README.md')}">すべての文書</a> · <a href="${pages.get('docs/Limitations.md')}">まだできないこと</a> · <a href="llms.txt">AI 向けの案内（llms.txt）</a></p>
</article>
`
}

function build() {
  const groups = documentGroups()
  const pages = pageMap(groups)
  const out = new Map()
  const readme = read('README.md')
  const tagline = /^\*\*(.+)\*\*$/m.exec(readme)?.[1] ?? 'Tsugumi'
  const lead = about(readme, tagline).join('')

  STORY.forEach((s, i) => {
    const marked = makeMarked(linker(s.source, s.out, pages))
    out.set(s.out, page({
      outRel: s.out, title: s.title ?? titleOf(s.source), source: s.source, markdown: markdownOut(s.source, pages), description: tagline,
      body: storyBody(s, marked), nav: sidebar(groups, pages, s.out), story: true,
      whole: s.sections ? pages.get(s.source) : null, neighbours: { next: STORY[i + 1]?.out, prev: STORY[i - 1]?.out }, gather: s.gather,
    }))
  })
  out.set('index.html', page({
    outRel: 'index.html', title: 'ドキュメント', source: 'README.md', markdown: markdownOut('README.md', pages), description: tagline,
    nav: sidebar(groups, pages, 'index.html'), home: homePage(groups, pages, tagline, lead),
  }))
  for (const [source, outRel] of pages) {
    if (out.has(outRel) || STORY.some((s) => s.out === outRel)) continue
    const marked = makeMarked(linker(source, outRel, pages))
    out.set(outRel, page({
      outRel, title: titleOf(source), source, markdown: markdownOut(source, pages), description: tagline,
      body: marked.parse(read(source)), nav: sidebar(groups, pages, outRel), story: false, toc: pageContents(marked.headings), embeds: read(source).includes('data-player='),
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
