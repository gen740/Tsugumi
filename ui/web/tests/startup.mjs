// Cross-process startup and discovery tests need no RPC service or sockets.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fork } from 'node:child_process'
import { on, once } from 'node:events'
import { pathToFileURL, fileURLToPath } from 'node:url'
import * as esbuild from 'esbuild'

const web = fileURLToPath(new URL('../', import.meta.url))
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-startup-'))
const bundle = path.join(dir, 'startup.mjs')
try {
  await esbuild.build({
    entryPoints: { startup: 'server/startup.ts', paths: 'server/paths.ts' },
    absWorkingDir: web, outdir: dir, outExtension: { '.js': '.mjs' },
    bundle: true, platform: 'node', format: 'esm',
  })
  const { startupLock } = await import(pathToFileURL(bundle).href)
  const workerPath = path.join(dir, 'contender.mjs')
  fs.writeFileSync(workerPath, `
    import { startupLock } from './startup.mjs'
    import { once } from 'node:events'
    const dir = process.argv[2]
    const acquisition = startupLock(dir)
    process.send('contending')
    await acquisition
    const held = once(process, 'message')
    process.send('locked')
    await held
  `)
  const lock = path.join(dir, 'startup.lock')
  const release = await startupLock(dir)
  const liveOwner = fs.readlinkSync(lock)
  const contender = fork(workerPath, [dir], { stdio: ['ignore', 'ignore', 'inherit', 'ipc'] })
  const exited = once(contender, 'exit')
  const messages = on(contender, 'message')
  const nextMessage = () => Promise.race([
    messages.next().then(({ value: [message] }) => message),
    exited.then(([code, signal]) => { throw new Error(`Lock contender exited: ${code ?? signal}`) }),
  ])
  try {
    assert.equal(await nextMessage(), 'contending')
    assert.equal(fs.readlinkSync(lock), liveOwner, 'a live owner keeps the lock')
    release()
    assert.equal(await nextMessage(), 'locked')
    contender.kill('SIGKILL')
    assert.deepEqual(await exited, [null, 'SIGKILL'])
    assert.equal(Number(fs.readlinkSync(lock).split('-')[0]), contender.pid)

    // The dead child's symlink is reaped with another acquisition pending.
    const recovered = [startupLock(dir), startupLock(dir)].map(async pending => {
      const release = await pending
      try {
        const owner = fs.readlinkSync(lock)
        assert.equal(Number(owner.split('-')[0]), process.pid)
        await Promise.resolve()
        assert.equal(fs.readlinkSync(lock), owner, 'the other contender cannot replace a held lock')
      } finally { release() }
    })
    await Promise.all(recovered)
    assert.throws(() => fs.lstatSync(lock), { code: 'ENOENT' })
  } finally {
    release()
    if (contender.exitCode === null && contender.signalCode === null) contender.kill('SIGKILL')
    await messages.return()
    await exited
  }

  const paths = path.join(dir, 'paths.mjs')
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
