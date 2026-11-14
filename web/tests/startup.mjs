// Cross-process startup and discovery tests need no RPC service or sockets.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { spawn } from 'node:child_process'
import { pathToFileURL, fileURLToPath } from 'node:url'
import * as esbuild from 'esbuild'

const web = fileURLToPath(new URL('../', import.meta.url))
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-startup-'))
const bundle = path.join(dir, 'startup.mjs')
try {
  await esbuild.build({ entryPoints: ['server/startup.ts'], absWorkingDir: web, outfile: bundle, bundle: true, platform: 'node', format: 'esm' })
  const worker = `
    import fs from 'node:fs';
    import { startupLock } from ${JSON.stringify(pathToFileURL(bundle).href)};
    const release = await startupLock(${JSON.stringify(dir)});
    const file = ${JSON.stringify(path.join(dir, 'count'))};
    const before = Number(fs.readFileSync(file, 'utf8'));
    await new Promise(r => setTimeout(r, 20));
    fs.writeFileSync(file, String(before + 1));
    release();
  `
  const run = () => new Promise((resolve, reject) => {
    const p = spawn(process.execPath, ['--input-type=module', '-e', worker], { stdio: ['ignore', 'ignore', 'pipe'] })
    let errors = ''
    p.stderr.on('data', b => { errors += b })
    p.on('error', reject)
    p.on('exit', code => code === 0 ? resolve() : reject(new Error(errors)))
  })
  fs.writeFileSync(path.join(dir, 'count'), '0')
  await Promise.all(Array.from({ length: 12 }, run))
  assert.equal(fs.readFileSync(path.join(dir, 'count'), 'utf8'), '12')
  // Reclaim an actual dead owner, with many simultaneous contenders.
  const dead = spawn(process.execPath, ['-e', ''], { stdio: 'ignore' })
  await new Promise(resolve => dead.on('exit', resolve))
  fs.symlinkSync(`${dead.pid}-00000000-0000-0000-0000-000000000000`, path.join(dir, 'startup.lock'))
  await Promise.all(Array.from({ length: 12 }, run))
  assert.equal(fs.readFileSync(path.join(dir, 'count'), 'utf8'), '24')
  assert.throws(() => fs.lstatSync(path.join(dir, 'startup.lock')), { code: 'ENOENT' })

  const paths = path.join(dir, 'paths.mjs')
  await esbuild.build({ entryPoints: ['server/paths.ts'], absWorkingDir: web, outfile: paths, bundle: true, platform: 'node', format: 'esm' })
  const { readServer, runningServer, serverBuild, serverRequest } = await import(pathToFileURL(paths).href)
  const file = path.join(dir, 'server.json')
  // The old shape is rejected, never reused or interpreted as a dead server.
  fs.writeFileSync(file, JSON.stringify({ pid: process.pid, port: 1, agentKey: 'a'.repeat(64) }))
  assert.throws(() => readServer(dir), /Invalid server.json/)
  const identity = serverBuild(bundle)
  const s = { pid: process.pid, port: 1234, agentKey: 'a'.repeat(64), build: identity }
  fs.writeFileSync(file, JSON.stringify(s))
  assert.deepEqual(readServer(dir), s)
  await assert.rejects(runningServer(dir, 'b'.repeat(64)), /different Tsugumi build.*Stop that host/)
  const originalFetch = globalThis.fetch
  const requests = []
  globalThis.fetch = async (url, options) => {
    requests.push({ url, options })
    return new Response(JSON.stringify({ build: identity }), { status: 200 })
  }
  try {
    assert.deepEqual(await runningServer(dir, identity), s)
    await serverRequest(s, '/api/page/open', { path: '/project with spaces' })
    assert.deepEqual(JSON.parse(requests.at(-1).options.body), { path: '/project with spaces' })
    assert.equal(requests.at(-1).options.headers['x-tsugumi-agent'], s.agentKey)
    globalThis.fetch = async () => { throw new Error('unreachable') }
    await assert.rejects(runningServer(dir, identity), /unreachable/)
  } finally { globalThis.fetch = originalFetch }
  fs.appendFileSync(bundle, '\n// rebuilt')
  assert.notEqual(serverBuild(bundle), identity)
  console.log('host startup and discovery: passed')
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
