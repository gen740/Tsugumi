import assert from 'node:assert/strict'
import { test } from 'node:test'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { catalog, linker, REPO, ROOT, SITE, STORY } from '../src/lib/documents.mjs'

const site = path.join(ROOT, 'build/docs')
const origin = 'https://example.test/Tsugumi/'
const { entries, markdownPages } = catalog()
const htmlFiles = ['index.html', ...entries.map(entry => entry.out)]
const read = relative => fs.readFileSync(path.join(site, relative), 'utf8')
const decode = text => text.replaceAll('&amp;', '&').replaceAll('&quot;', '"')

function references(html) {
  return [...html.matchAll(/<(?:a|img|link|script|source)\b[^>]*>/g)].flatMap(([tag]) =>
    [...tag.matchAll(/\b(?:href|src)="([^"]+)"/g)].map(([, value]) => decode(value)))
}

function localURL(relative, reference) {
  const url = new URL(reference, origin + relative)
  if (url.origin !== new URL(origin).origin) return null
  assert.ok(url.pathname.startsWith('/Tsugumi/'), `${relative}: URL escapes the Pages subpath: ${reference}`)
  return { file: decodeURIComponent(url.pathname.slice('/Tsugumi/'.length)), hash: decodeURIComponent(url.hash.slice(1)) }
}

test('every source document has one catalog entry and a Markdown download', () => {
  const sources = entries.filter(entry => !entry.story?.sections).map(entry => entry.source)
  assert.equal(new Set(sources).size, sources.length)
  for (const file of fs.readdirSync(path.join(ROOT, 'docs')).filter(file => file.endsWith('.md'))) {
    assert.ok(sources.includes(`docs/${file}`), `${file} is missing from docs/README.md`)
  }
  for (const [source, output] of markdownPages) {
    assert.ok(read(output).length > 0, `${source} has no Markdown download`)
  }
  assert.ok(!fs.existsSync(path.join(ROOT, 'docs/pages')))
  assert.ok(!fs.existsSync(path.join(ROOT, 'docs/index.md')))
})

test('HTML links, anchors and assets resolve under the GitHub Pages subpath', () => {
  const failures = []
  for (const file of htmlFiles) {
    const html = read(file)
    assert.match(html, /<html lang="en"/)
    assert.doesNotMatch(html, /<astro-island\b/, 'ordinary document pages do not need client hydration')
    for (const reference of references(html)) {
      const target = localURL(file, reference)
      if (!target) continue
      if (!fs.existsSync(path.join(site, target.file))) failures.push(`${file}: missing ${reference}`)
      else if (target.hash && target.file.endsWith('.html')) {
        const ids = [...read(target.file).matchAll(/\bid="([^"]+)"/g)].map(([, id]) => decode(id))
        if (!ids.includes(target.hash)) failures.push(`${file}: missing anchor ${reference}`)
      }
    }
  }
  assert.deepEqual(failures, [])
})

test('repository guides stay on GitHub and outside the published catalog', () => {
  const { pages } = catalog()
  const link = linker('README.md', 'pages/readme.html', pages)
  for (const source of ['AGENTS.md', 'RELEASING.md', 'tests/README.md', 'docs/site/README.md']) {
    assert.ok(!pages.has(source), source)
    assert.equal(link(source), `${REPO}/blob/main/${source}`)
    assert.ok(!read('llms.txt').includes(`${SITE}${source}`), source)
  }
  assert.ok(!fs.existsSync(path.join(site, 'site/README.md')))
})

test('published JavaScript modules parse without a browser', () => {
  for (const file of fs.readdirSync(path.join(site, 'site/js')).filter(file => file.endsWith('.js'))) {
    execFileSync(process.execPath, ['--check', path.join(site, 'site/js', file)])
  }
})

test('story figures and navigation survive Markdown rendering', () => {
  for (const story of STORY) {
    const html = read(story.out)
    assert.ok(html.slice(0, 400).includes('class="story"'), 'the app detects stories in the first 400 bytes')
    assert.match(html, /data-mode="cards"/)
    for (const [, figure] of story.figures) {
      if (typeof figure === 'string') assert.ok(html.includes(`data-fig="${figure}"`), figure)
      else assert.ok(html.includes(figure.alt))
    }
  }
  assert.match(read('pages/cardguide.html'), /data-player="card-guide"/)
  assert.match(read('index.html'), /data-demo="casa"/)
})

test('llms.txt lists existing downloadable documents', () => {
  const links = [...read('llms.txt').matchAll(/\]\(https:\/\/gen740.github.io\/Tsugumi\/([^)]*)\)/g)]
  assert.ok(links.length >= markdownPages.size - 2)
  for (const [, file] of links) assert.ok(fs.existsSync(path.join(site, file)), file)
})

test('the app help bundle contains all local script, style and image dependencies', () => {
  const destination = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-docs-test-'))
  try {
    execFileSync('bash', [path.join(ROOT, 'macOS/tools/bundle-docs.sh'), destination])
    assert.deepEqual(fs.readdirSync(path.join(destination, 'pages')).sort(), STORY.map(story => path.basename(story.out)).sort())
    for (const story of STORY) {
      const html = fs.readFileSync(path.join(destination, story.out), 'utf8')
      for (const tag of html.matchAll(/<(?:img|script|link)\b[^>]*>/g)) {
        if (tag[0].includes('rel="canonical"') || tag[0].includes('rel="alternate"') || tag[0].includes('rel="describedby"')) continue
        for (const reference of references(tag[0])) {
          const target = localURL(story.out, reference)
          if (target) assert.ok(fs.existsSync(path.join(destination, target.file)), `${story.out}: ${reference}`)
        }
      }
    }
    for (const name of ['reader', 'art', 'scenes', 'scenes2']) {
      const file = path.join(destination, `site/js/${name}.js`)
      const code = fs.readFileSync(file, 'utf8')
      for (const [, relative] of code.matchAll(/(?:from\s*|import\s*)['"](\.[^'"]+)['"]/g)) {
        assert.ok(fs.existsSync(path.resolve(path.dirname(file), relative)), relative)
      }
    }
    const css = fs.readFileSync(path.join(destination, 'site/site.css'), 'utf8')
    for (const [, reference] of css.matchAll(/url\(['"]?([^)'"\s]+)['"]?\)/g)) {
      if (/^(data:|https?:)/.test(reference)) continue
      assert.ok(fs.existsSync(path.resolve(destination, 'site', reference)), reference)
    }
  } finally {
    fs.rmSync(destination, { recursive: true, force: true })
  }
})
