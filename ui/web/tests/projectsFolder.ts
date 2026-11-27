// A new project's folder picker starts in the projects folder, made when it is missing.
import assert from 'node:assert/strict'
import { projectsFolder } from '../src/api'

const previousFetch = globalThis.fetch
function host(existing: string[], { refuse = false } = {}) {
  const folders = new Set(existing)
  const made: string[] = []
  globalThis.fetch = (async (url: string, init?: RequestInit) => {
    const answer = (status: number, value: unknown) => ({ ok: status === 200, status, json: async () => value }) as Response
    if (url.startsWith('/api/fs?path=')) {
      const path = decodeURIComponent(url.slice('/api/fs?path='.length))
      return folders.has(path) ? answer(200, { path, parent: null, project: false, entries: [] }) : answer(400, { error: 'missing' })
    }
    assert.equal(url, '/api/fs/mkdir')
    const { parent, name } = JSON.parse(String(init?.body))
    const path = `${parent}/${name}`
    if (refuse || !folders.has(parent) || folders.has(path)) return answer(400, { error: 'cannot create' })
    folders.add(path)
    made.push(path)
    return answer(200, { path, parent, project: false, entries: [] })
  }) as typeof fetch
  return made
}
const status = { home: '/Users/me', documents: '/Users/me/Documents/Tsugumi' }
try {
  let made = host(['/Users', '/Users/me', '/Users/me/Documents', '/Users/me/Documents/Tsugumi'])
  assert.equal(await projectsFolder(status), status.documents)
  assert.deepEqual(made, [], 'an existing projects folder is used as it is')

  made = host(['/Users', '/Users/me'])
  assert.equal(await projectsFolder(status), status.documents)
  assert.deepEqual(made, ['/Users/me/Documents', '/Users/me/Documents/Tsugumi'], 'missing folders are made, outermost first')

  made = host(['/Users', '/Users/me'], { refuse: true })
  assert.equal(await projectsFolder(status), status.home, 'a folder that cannot be made falls back to home')
} finally { globalThis.fetch = previousFetch }
console.log('projects folder: passed')
