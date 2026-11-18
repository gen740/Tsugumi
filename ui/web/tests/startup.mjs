// Cross-process startup and discovery tests need no RPC service or sockets.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fork, spawn } from 'node:child_process'
import { pathToFileURL, fileURLToPath } from 'node:url'
import * as esbuild from 'esbuild'

const web = fileURLToPath(new URL('../', import.meta.url))
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-startup-'))
const bundle = path.join(dir, 'startup.mjs')
try {
  await esbuild.build({ entryPoints: ['server/startup.ts'], absWorkingDir: web, outfile: bundle, bundle: true, platform: 'node', format: 'esm' })
  const workerPath = path.join(dir, 'contender.mjs')
  fs.writeFileSync(workerPath, `
    import fs from 'node:fs';
    import { startupLock } from ${JSON.stringify(pathToFileURL(bundle).href)};
    const permission = new Promise(resolve => process.once('message', resolve));
    const acquisition = startupLock(${JSON.stringify(dir)});
    process.send('contending');
    const release = await acquisition;
    const file = ${JSON.stringify(path.join(dir, 'count'))};
    const before = Number(fs.readFileSync(file, 'utf8'));
    await permission;
    fs.writeFileSync(file, String(before + 1));
    release();
    process.disconnect();
  `)
  const run = () => {
    const contenders = []
    const processes = []
    const exits = Array.from({ length: 12 }, () => new Promise((resolve, reject) => {
      const p = fork(workerPath, [], { stdio: ['ignore', 'ignore', 'pipe', 'ipc'] })
      processes.push(p)
      let errors = ''
      p.stderr.on('data', b => { errors += b })
      p.on('error', reject)
      p.on('exit', code => code === 0 ? resolve() : reject(new Error(errors)))
      p.on('message', event => {
        if (event === 'contending') contenders.push(p)
        // Hold the first lock until every process has reached the attempt.
        if (contenders.length === exits.length) {
          for (const child of contenders) if (child.connected) child.send('release')
          contenders.length = 0
        }
      })
    }))
    return Promise.all(exits).catch(error => {
      for (const child of processes) if (child.exitCode === null) child.kill()
      throw error
    })
  }
  fs.writeFileSync(path.join(dir, 'count'), '0')
  await run()
  assert.equal(fs.readFileSync(path.join(dir, 'count'), 'utf8'), '12')
  // Reclaim an actual dead owner, with many simultaneous contenders.
  const dead = spawn(process.execPath, ['-e', ''], { stdio: 'ignore' })
  await new Promise(resolve => dead.on('exit', resolve))
  fs.symlinkSync(`${dead.pid}-00000000-0000-0000-0000-000000000000`, path.join(dir, 'startup.lock'))
  await run()
  assert.equal(fs.readFileSync(path.join(dir, 'count'), 'utf8'), '24')
  assert.throws(() => fs.lstatSync(path.join(dir, 'startup.lock')), { code: 'ENOENT' })

  const paths = path.join(dir, 'paths.mjs')
  await esbuild.build({ entryPoints: ['server/paths.ts'], absWorkingDir: web, outfile: paths, bundle: true, platform: 'node', format: 'esm' })
  const { readServer, runningServer, serverBuild, serverRequest } = await import(pathToFileURL(paths).href)
  const file = path.join(dir, 'server.json')
  const identity = serverBuild(bundle)
  const s = { pid: process.pid, port: 1234, agentKey: 'a'.repeat(64), build: identity }
  fs.writeFileSync(file, JSON.stringify(s))
  assert.deepEqual(readServer(dir), s)
  await assert.rejects(runningServer(dir, 'b'.repeat(64)), Error)
  assert.deepEqual(readServer(dir), s)
  const originalFetch = globalThis.fetch
  const requests = []
  globalThis.fetch = async (url, options) => {
    requests.push({ url, options })
    return new Response(JSON.stringify({ build: identity }), { status: 200 })
  }
  try {
    assert.deepEqual(await runningServer(dir, identity), s)
    const opened = await serverRequest(s, '/api/page/open', { path: '/project with spaces' })
    assert.equal(opened.status, 200)
    assert.deepEqual(JSON.parse(requests.at(-1).options.body), { path: '/project with spaces' })
    assert.equal(requests.at(-1).options.headers['x-tsugumi-agent'], s.agentKey)
    const cancel = new AbortController()
    globalThis.fetch = async (_url, options) => {
      assert.ok(options.signal, 'an in-flight request can be cancelled')
      return new Promise((_, reject) => {
        options.signal.addEventListener('abort', () => reject(options.signal.reason), { once: true })
      })
    }
    const pending = serverRequest(s, '/api/agent/call', { name: 'run_status', arguments: { wait_seconds: 300 } }, cancel.signal)
    const cancelled = new Error('caller cancelled')
    cancel.abort(cancelled)
    await assert.rejects(pending, error => error === cancelled)
    globalThis.fetch = async () => { throw new Error('unreachable') }
    assert.deepEqual(await runningServer(dir, identity), s)
  } finally { globalThis.fetch = originalFetch }
  fs.appendFileSync(bundle, '\n// rebuilt')
  assert.notEqual(serverBuild(bundle), identity)
  console.log('host startup and discovery: passed')
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
