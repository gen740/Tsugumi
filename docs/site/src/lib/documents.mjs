import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { Marked } from 'marked'

const ROOT = process.env.TSUGUMI_DOCS_ROOT ?? fileURLToPath(new URL('../../../../', import.meta.url))
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
const OFF_THE_LIST = new Set(['docs/INSTALL.md', 'docs/Demos.md', 'docs/Limitations.md'])

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

const read = (rel) => fs.readFileSync(path.join(ROOT, rel), 'utf8')
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')

const titleOf = (rel) => /^# (.+)$/m.exec(read(rel))?.[1].replace(/[`*]/g, '') ?? rel

/** Every other document: docs/README.md is the list, grouped as it groups them. */
const notes = new Map()
function documentGroups() {
  notes.clear()
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
    // Tooling READMEs under docs/site belong on GitHub, outside published assets.
    if (rel.startsWith('docs/assets/') || (rel.startsWith('docs/site/') && !rel.endsWith('.md'))) return up(rel.slice('docs/'.length))
    const abs = path.join(ROOT, rel)
    const kind = fs.existsSync(abs) && fs.statSync(abs).isDirectory() ? 'tree' : 'blob'
    return `${REPO}/${kind}/main/${rel}${tail}`
  }
}

function storyMarkdown(story) {
  let tokens = new Marked().lexer(read(story.source))
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
    tokens.unshift({ type: 'heading', depth: 1, raw: `# ${story.title}`, text: story.title, tokens: [], })
  }
  tokens = tokens.filter((t) => !(t.type === 'paragraph' && t.raw.startsWith('[Back to the README]')) && t.type !== 'space')
  const after = new Map()
  for (const [phrase, what, caption] of story.figures ?? []) {
    const i = tokens.findIndex((t) => t.raw.includes(phrase))
    if (i < 0) throw new Error(`${story.source}: no block has "${phrase}" (figure ${what.image ?? what})`)
    after.set(i, [...(after.get(i) ?? []), { what, caption }])
  }
  let markdown = ''
  tokens.forEach((t, i) => {
    markdown += t.raw + '\n\n'
    for (const { what, caption } of after.get(i) ?? []) {
      markdown += what.image
        ? `<figure class="shot"><img src="${esc(path.relative(path.dirname(story.source), `docs/${what.image}`))}" alt="${esc(what.alt)}" width="${what.width}" height="${what.height}" loading="lazy"><figcaption>${esc(caption)}</figcaption></figure>\n\n`
        : `<figure class="ex-fig" data-fig="${what}"><div class="ex-fig-pin"><div class="ex-fig-space"></div>${caption ? `<figcaption>${esc(caption)}</figcaption>` : ''}</div></figure>\n\n`
    }
  })
  return markdown
}

/** Where a document's Markdown is on the site: next to its page; the README at the top. */
const markdownOut = (source, pages) => (source === 'README.md' ? 'index.md' : pages.get(source).replace(/\.html$/, '.md'))

/** A document as it is, with links that work on the site (outside code blocks). */
function rewriteLinks(markdown, link) {
  // Fenced examples are source code, not navigation. Leave their contents intact.
  return markdown.split(/(^```[\s\S]*?^```)/m).map((part, i) => (i % 2
    ? part
    : part.replace(/\]\(([^)\s]+)\)/g, (_, to) => `](${link(to)})`).replace(/\b(src|href)="([^"]+)"/g, (_, a, to) => `${a}="${link(to)}"`))).join('')
}

function markdownPage(source, outRel, mdPages) {
  return rewriteLinks(read(source), linker(source, outRel, mdPages))
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
    'The documents are written for both people and AI. The URLs below are the Markdown version of each page.', '',
    '## Overview', '', `- [README](${SITE}index.md): the problem, how to use it, and the real screens explained`, '',
  ]
  for (const g of groups) {
    const docs = g.docs.filter((d) => !SECONDARY.includes(d))
    out.push(`## ${g.title}`, '', ...docs.map(item), '')
  }
  out.push('## Repository', '',
    `- [AGENTS.md](${RAW}/AGENTS.md): a map and instructions for agents working in this repository`,
    `- [The Card specification](${RAW}/agent/Reference/card-guide.md): the same document the app's Agent reads`,
    `- [The RPC contract](${RAW}/protocol/tsugumi.fbs): the source of the contract the UI, the Web and the Agent use`, '')
  out.push('## Optional', '', ...SECONDARY.map(item), '')
  return out.join('\n')
}


function catalog() {
  const groups = documentGroups()
  const pages = pageMap(groups)
  const markdownPages = new Map([...pages.keys()].map(source => [source, markdownOut(source, pages)]))
  const entries = [...pages].map(([source, out]) => ({ source, out }))
  for (const story of STORY) {
    const entry = entries.find(entry => entry.out === story.out)
    if (entry) entry.story = story
    else entries.push({ source: story.source, out: story.out, story })
  }
  return { groups, pages, markdownPages, entries }
}

function introduction() {
  const readme = read('README.md')
  const tagline = /^\*\*(.+)\*\*$/m.exec(readme)?.[1] ?? 'Tsugumi'
  return { tagline, lead: about(readme, tagline).join(' ') }
}

export { ROOT, REPO, SITE, STORY, GALLERY, DOWNLOAD, OFF_THE_LIST, notes,
  read, esc, titleOf, linker, rewriteLinks, markdownPage, markdownOut, llmsTxt,
  storyMarkdown, catalog, introduction }
