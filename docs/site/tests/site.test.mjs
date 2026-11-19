import assert from 'node:assert/strict'
import { test } from 'node:test'
import fs from 'node:fs'
import path from 'node:path'
import { catalog, linker, REPO, ROOT, STORY } from '../src/lib/documents.mjs'

const site = path.join(ROOT, 'build/docs')
const origin = 'https://example.test/Tsugumi/'
const { entries } = catalog()
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

test('HTML links, anchors and assets resolve under the GitHub Pages subpath', () => {
  const failures = []
  for (const file of htmlFiles) {
    const html = read(file)
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
  for (const [, file] of read('llms.txt').matchAll(/\]\(https:\/\/gen740.github.io\/Tsugumi\/([^)]*)\)/g)) {
    if (!fs.existsSync(path.join(site, file))) failures.push(`llms.txt: missing ${file}`)
  }
  assert.deepEqual(failures, [])
})

test('repository guides stay on GitHub and outside the published catalog', () => {
  const { pages } = catalog()
  const link = linker('README.md', 'pages/readme.html', pages)
  for (const source of ['AGENTS.md', 'RELEASING.md', 'tests/README.md', 'docs/site/README.md']) {
    assert.equal(link(source), `${REPO}/blob/main/${source}`)
  }
})

test('the app can detect published stories', () => {
  for (const story of STORY) {
    const html = read(story.out)
    assert.ok(html.slice(0, 400).includes('class="story"'), 'the app detects stories in the first 400 bytes')
  }
})

test('published player data points to available images', () => {
  const index = JSON.parse(read('site/player-data/index.json'))
  for (const { id } of index.demos) {
    const demo = JSON.parse(read(`site/player-data/${id}.json`))
    for (const output of Object.values(demo.outputs)) {
      if (output.src) assert.ok(fs.existsSync(path.join(site, 'site/player-data', output.src)), `${id}: ${output.src}`)
    }
  }
})
