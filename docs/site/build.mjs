// Writes the documentation site from the repository's Markdown: docs/index.html
// and docs/pages/*.html, for GitHub Pages serving docs/. The Markdown files stay
// the single source. The story pages ("Learn about Tsugumi") get figures set in after
// phrases of their text; a phrase or section that is gone fails the build.
// For agents, each page also has its Markdown next to it (pages/x.md, with links
// that work on the site) and docs/llms.txt lists them (https://llmstxt.org/).
// Run after changing a document:
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
const SECONDARY = ['docs/Lock.md', 'docs/LicenseAudit.md']

/** Pages read with the thread and the moving figures, in the order the slide show goes through them. */
const STORY = [
  {
    out: 'pages/problem.html', source: 'README.md', title: 'The problem Tsugumi addresses', navTitle: 'The problem', know: true,
    // A section, or its beginning: upTo names the first block left out.
    sections: [
      { title: 'Are you using AI in your research as much as you could?' },
      { title: 'The flow of processing as a common language for people and AI', upTo: 'This idea starts from research' },
      { title: 'What we want to ask of AI should be simple' },
      { title: 'Draw the shape of the processing. Implement it with AI. Run it and check.' },
    ],
    figures: [
      ['**To hand work to an AI', 'summit', 'Leave the path the AI climbed as footholds you can check, and people can climb it too.'],
      ['What these share is', 'wall', 'The more the AI\'s output piles up, the higher the wall between people and AI grows.'],
      ['Tsugumi shares all three on the same Workflow', 'thread', 'Tsugumi joins people and AI with one thread, over the wall that piled up.'],
      ['Download the observation, split it', 'casaSky', 'Take Cassiopeia A: the remnant of a supernova whose light reached Earth a little over three hundred years ago, still expanding at thousands of kilometres per second. Split its observation into three by energy and overlay them, and you have a three-colour image.'],
      ['three boxes on paper', 'sketch', 'Three boxes, drawn on paper.'],
      ['macOS app for assembling', 'draft', 'What if what you wrote took shape just as it is? The three boxes become Drafts.'],
      ['**Check.** Run the graph', 'run', 'The Agent implements the Drafts and runs them, and the photon counts per band and the RGB image appear on the Canvas (the bundled Cas A · Chandra RGB demo).'],
      ['**Check.** Run the graph', { image: 'assets/screenshots/workflow.png', width: 1440, height: 900, alt: 'A Canvas that makes an RGB image and an energy spectrum from an X-ray observation of Cas A' }, 'The real screen. Run it on a deep observation, and even how the spectrum\'s lines match the image\'s colours can be checked on the same Canvas.'],
    ],
  },
  {
    out: 'pages/philosophy.html', source: 'docs/Philosophy.md', know: true,
    figures: [
      ['I would like to call research done this way', 'driven', 'People ask the questions. AI runs the trials. People give them meaning, again.'],
      ['Can the speed at which people understand', 'outpace', 'How fast AI tries, and how fast people understand. The gap keeps widening.'],
      ['A person who has been away from driving', 'driver', 'Someone who had handed over the wheel is suddenly asked to take it back.'],
      ['I feel that using AI in research has exactly', 'grasp', 'Being within reach is not the same as understanding. In driving, and in research.'],
      ['Check how the samples were chosen', 'curve', 'A beautiful line through the chosen points only, and the line with the excluded points put back.'],
      ['That is why I want the process of trial and error', 'partial', 'Change the conditions of the Card you were curious about, and run again from there.'],
      ['a single thread drawn from the temari becomes the wire', 'name', 'A single thread drawn from the temari becomes the wire between Card and Card.'],
      ['It comes to Japan as a winter bird', 'walk', 'A few hops, a stop, and it stands up straight to look around.'],
      ['In an age that tells results with ever more words', 'hush', 'Saying little, it offers the accumulated attempts.'],
      ['A first-year PhD student at the University of Tokyo', 'author', 'Yasuda Auditorium, the University of Tokyo. Research that reads the universe with AI, like joining up the stars.'],
    ],
  },
  {
    out: 'pages/rationale.html', source: 'docs/Rationale.md',
    figures: [
      ['Hand an analysis to AI and the trials go fast', 'oneGraph', 'What people draw, what AI edits and what actually runs become one.'],
      ['In Tsugumi you can run only the Cards you choose', 'partial', 'From the Card you fixed, try it with the saved inputs. Finally, run the whole.'],
      ['Tsugumi versions the code and the Workflow', 'branches', 'From one result to another hypothesis. Every branch can be followed back.'],
    ],
  },
  {
    out: 'pages/vision.html', source: 'docs/Vision.md',
    figures: [
      ['The learner can change inputs and parameters', 'teach', 'Change the conditions yourself, and see how the conclusion moves.'],
      ['For example, when handing over a monthly aggregation', 'handoff', 'Hand over the intention, the code and the environment together, and the next person keeps trying from there.'],
    ],
  },
]

/** What the list leaves out: reached from Home (the download, the gallery) or from the list of documents. */
const OFF_THE_LIST = new Set(['INSTALL.md', 'docs/Demos.md', 'docs/Limitations.md'])

/** The demos of the gallery on Home, by what they are about, in plain words (demos/index.json has the rest). */
const GALLERY = [
  ['The basics', [
    ['report', 'Data to report', 'Generate data, aggregate it and write a report. Change a step in the middle and rerun from there.'],
    ['collatz', 'Collatz (C++ / Python / Julia / R)', 'The same computation in four languages, compared while following its branch and loop.'],
    ['plots', 'Plots', 'matplotlib code written for a window becomes the Card\'s display as it is.'],
    ['live-stream', 'Live Stream', 'A time series plotted while the run is still going.'],
  ]],
  ['Public data', [
    ['casa', 'Cas A · Chandra RGB', 'An X-ray observation split into three energy bands for an RGB image, with the spectrum of the same photons to match lines to colours.'],
    ['gw150914', 'GW150914', 'The chirp of a gravitational wave, extracted from public data and plotted.'],
    ['protein-6lu7', '6LU7', 'A protein\'s 3D structure, fetched to look at its binding pocket.'],
    ['exoplanet-transit', 'Kepler-6 b', 'A light curve folded to show an exoplanet\'s transit.'],
    ['seismic-trench', 'Japan Trench', 'A cross-section of hypocentres and the b-value, from an earthquake catalogue.'],
  ]],
  ['Machine learning and computation', [
    ['mnist-classifiers', 'MNIST · comparing classifiers', 'The real MNIST fed to seven classifiers at once, comparing accuracy and time.'],
    ['newton', 'Newton\'s method', 'A map of the complex plane showing how the starting point decides where it goes and how fast.'],
    ['max-flow', 'Maximum flow and minimum cut', 'On a made-up network, which edge to widen to increase the flow.'],
    ['pharmacokinetics', 'Pharmacokinetics (a made-up drug)', 'Teaching material comparing how concentration moves with the dosing interval. Not medical advice.'],
    ['dose-response', 'Dose–response curve (synthetic data)', 'A fit and its confidence interval, checked on data whose true values are known.'],
    ['molecules', 'Molecular properties', 'Properties and structural formulas from SMILES, with RDKit.'],
    ['cell-count', 'Counting cells in images', 'A counting method scored on synthetic images with known answers, then applied to real images of nuclei.'],
  ]],
  ['Statistics in R', [
    ['survival', 'Survival analysis', 'How much the same hazard ratio moves with the subjects and the adjustment.'],
    ['meta-analysis', 'Meta-analysis: the BCG vaccine', 'How the conclusion moves with the pooling method and the trials left out.'],
    ['rna-seq', 'RNA-seq: airway', 'With DESeq2 and edgeR, how the number of differentially expressed genes changes with the threshold.'],
    ['simpson', 'A correlation that reverses when stratified', 'Two real datasets in which the whole and its strata point in opposite directions.'],
  ]],
  ['Simulation in Julia', [
    ['battery', 'Scheduling a battery', 'Charging and discharging optimised by dynamic programming, under made-up prices.'],
    ['gillespie', 'Fluctuations in chemical reactions', 'A Gillespie simulation checked against theory.'],
    ['heat-inverse', 'An inverse problem in heat conduction', 'The naive inverse breaks down; regularisation recovers the answer.'],
    ['ecology-models', 'Comparing ecological models', 'Predator–prey models fitted to fur-trade records and compared.'],
  ]],
  ['Left to the Agent', [
    ['dynamics-draft', 'Dynamical systems gallery', 'The Agent splits six Drafts among helpers and implements them at the same time.'],
    ['mnist-classifiers-draft', 'MNIST · comparing classifiers', 'Helpers implement seven classifiers at the same time, and the Agent merges them into a comparison.'],
  ]],
]
const DOWNLOAD = `${REPO}/releases/latest`

const ICON = {
  github: '<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27s1.36.09 2 .27c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8Z"/></svg>',
  panel: '<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.4"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><path d="M6 2.5v11"/></svg>',
  download: '<svg viewBox="0 0 16 16" width="15" height="15" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M8 2v8M4.5 7 8 10.5 11.5 7M2.5 14h11"/></svg>',
}

const read = (rel) => fs.readFileSync(path.join(ROOT, rel), 'utf8')
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')

/** The same anchors as GitHub's. */
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
      // Lines of a paragraph in Japanese (should any be left) join without the space a line break would show.
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
  tokens = tokens.filter((t) => !(t.type === 'paragraph' && t.raw.startsWith('[Back to the README]')) && t.type !== 'space')
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
  return `<details class="toc" open><summary>Contents</summary><ul>${items}</ul></details>
<script>{ const fold = document.currentScript.previousElementSibling, narrow = matchMedia('(max-width: 1240px)'), set = () => { fold.open = !narrow.matches }; set(); narrow.addEventListener('change', set) }</script>
`
}

function sidebar(groups, pages, outRel) {
  const href = (to) => (to === outRel ? '#' : path.relative(path.dirname(outRel), to).split(path.sep).join('/'))
  const item = (to, title) => `<li${to === outRel ? ' class="on" aria-current="page"' : ''}><a href="${href(to)}">${esc(title)}</a></li>`
  let out = `<div class="nav-group"><h2>Learn about Tsugumi</h2><ul>`
  for (const s of STORY.filter((x) => x.know)) out += item(s.out, s.navTitle ?? s.title ?? titleOf(s.source))
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
    'The documents are written for both people and AI. The URLs below are the Markdown version of each page. How to check each claim is in "Claims and how to check them", with the code, tests and commands.', '',
    '## Overview', '', `- [README](${SITE}index.md): the problem, how to use it, and the real screens explained`, '',
  ]
  for (const g of groups) {
    const docs = g.docs.filter((d) => !SECONDARY.includes(d))
    out.push(`## ${g.title}`, '', ...docs.map(item), '')
  }
  out.push('## Repository', '',
    `- [AGENTS.md](${RAW}/AGENTS.md): a map, the claims and where to check them, and instructions for agents working in this repository`,
    `- [The Card specification](${RAW}/agent/Reference/card-guide.md): the same document the app's Agent reads`,
    `- [The RPC contract](${RAW}/protocol/tsugumi.fbs): the source of the contract the UI, the Web and the Agent use`, '')
  out.push('## Optional', '', ...SECONDARY.map(item), '')
  return out.join('\n')
}

function page({ outRel, title, source, markdown, description, body, nav, story, home, whole, toc, neighbours, embeds, gather }) {
  const asset = (p) => path.relative(path.dirname(outRel), p).split(path.sep).join('/')
  const canonical = SITE + (outRel === 'index.html' ? '' : outRel)
  return `<!doctype html>
<!-- Generated by docs/site/build.mjs from ${source}: do not edit. -->
<html lang="en"${story ? ' class="story"' : ''}>
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
<button type="button" class="side-open" aria-label="Open the sidebar" title="Open the sidebar">${ICON.panel}</button>
<div class="layout">
<nav class="side" aria-label="Documents">
  <div class="brand"><a href="${asset('index.html')}"><img src="${asset('assets/tsugumi-icon.svg')}" alt="" width="28" height="28">Tsugumi</a><span class="brand-links"><a class="repo" href="${REPO}" rel="noopener">${ICON.github}GitHub</a><button type="button" class="side-close" aria-label="Close the sidebar" title="Close the sidebar">${ICON.panel}</button></span></div>
  <details class="nav-fold" open><summary>Documents</summary>${nav}</details>
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
${home ?? `${story ? '<div class="mode" role="group" aria-label="How to read"><button type="button" data-mode="read" aria-pressed="true">Read</button><button type="button" data-mode="cards" aria-pressed="false">Slide show</button></div>' : ''}
<div class="reading">
${toc ?? ''}<article class="doc"${gather ? ' data-gather=""' : ''}>
${body}</article>
</div>
<p class="source">${whole
    ? `<a href="${REPO}/blob/main/${source}" rel="noopener"><code>${esc(source)}</code></a> (<a href="${asset(whole)}">the whole document</a> · <a href="${asset(markdown)}">Markdown</a>)`
    : `<a href="${REPO}/blob/main/${source}" rel="noopener"><code>${esc(source)}</code></a> (<a href="${asset(markdown)}">Markdown</a>)`}</p>
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
  const doorText = { 'Getting started': 'Start using it', 'The ideas': 'Why it takes this shape', 'How it works': 'What is inside', 'Development': 'Build and maintain Tsugumi' }
  const know = STORY.filter((s) => s.know)
  const row = (to, title, note) => `<li><a href="${here(to)}"><b>${esc(title)}</b>${note ? `<span>${esc(note)}</span>` : ''}</a></li>`
  const shortNote = (d) => (notes.get(d) ?? '').replace(/^(「[^」]+」——|[^:：]{1,14}[:：]\s*)/, '').replace(/`/g, '').replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
  const door = (id, name, sub, items) => `<section class="door" data-door="${id}"><header><i>&lt;/&gt;</i><h2>${esc(name)}</h2><small>${esc(sub)}</small></header><ul>${items}</ul></section>`
  const doors = [
    door('know', 'Learn about Tsugumi', 'What problem it takes on, and how', know.map((s) => row(s.out, s.title ?? titleOf(s.source), s.sections ? 'The wall and the thread, from three boxes to Cas A' : shortNote(s.source))).join('')),
    ...groups.map((g) => {
      const docs = g.docs.filter((d) => !OFF_THE_LIST.has(d) && !know.some((s) => s.source === d))
      return door(g.title, g.title, doorText[g.title] ?? '', docs.map((d) => row(pages.get(d), titleOf(d), shortNote(d))).join(''))
    }),
  ].join('')
  const tile = ([id, title, text]) => `<button type="button" class="demo-tile" id="demo-${id}" data-demo="${id}"><span class="demo-thumb"></span><b>${esc(title)}</b><span>${esc(text)}</span></button>`
  const tiles = GALLERY.map(([name, demos]) => `<h3>${esc(name)}</h3><div class="demo-grid">${demos.map(tile).join('')}</div>`).join('')
  return `<article class="home">
<header class="home-hero">
  <div class="home-copy">
    <h1>Tsugumi</h1>
    <p class="tagline">${esc(tagline)}</p>
    <p class="lead">${esc(lead)}</p>
    <div class="hero-actions">
      <a class="cta" href="${DOWNLOAD}">${ICON.download}Download for Mac</a>
      <a class="cta quiet" href="#demos">See the demos without installing</a>
    </div>
    <p class="home-note">Runs on macOS, Linux and Windows (Windows through WSL2. The app needs Apple silicon and macOS 26 or later) · <a href="${pages.get('INSTALL.md')}">Installation and development builds</a></p>
  </div>
  <div class="home-perch" aria-hidden="true"></div>
</header>
<div class="doors">${doors}</div>
<section class="gallery" id="demos">
  <h2>Run the demos, right here</h2>
  <p>The bundled demos. Without installing anything, you can see here how their Cards connect and how they run. Click one to open it large. Each is explained in <a href="${pages.get('docs/Demos.md')}">The bundled demos</a>.</p>
  ${tiles}
</section>
<p class="home-foot"><a href="${pages.get('README.md')}">README</a> · <a href="${pages.get('docs/README.md')}">All documents</a> · <a href="${pages.get('docs/Limitations.md')}">Plans</a> · <a href="llms.txt">A guide for AI (llms.txt)</a></p>
</article>
`
}

function build() {
  const groups = documentGroups()
  const pages = pageMap(groups)
  const out = new Map()
  const readme = read('README.md')
  const tagline = /^\*\*(.+)\*\*$/m.exec(readme)?.[1] ?? 'Tsugumi'
  const lead = about(readme, tagline).join(' ')

  STORY.forEach((s, i) => {
    const marked = makeMarked(linker(s.source, s.out, pages))
    out.set(s.out, page({
      outRel: s.out, title: s.title ?? titleOf(s.source), source: s.source, markdown: markdownOut(s.source, pages), description: tagline,
      body: storyBody(s, marked), nav: sidebar(groups, pages, s.out), story: true,
      whole: s.sections ? pages.get(s.source) : null, neighbours: { next: STORY[i + 1]?.out, prev: STORY[i - 1]?.out }, gather: s.gather,
    }))
  })
  out.set('index.html', page({
    outRel: 'index.html', title: 'Documentation', source: 'README.md', markdown: markdownOut('README.md', pages), description: tagline,
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
