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
  } finally {
    release()
    if (contender.exitCode === null && contender.signalCode === null) contender.kill('SIGKILL')
    await messages.return()
    await exited
  }

  const paths = path.join(dir, 'paths.mjs')
  const { serverBuild, serverRequest } = await import(pathToFileURL(paths).href)
  const identity = serverBuild(bundle)
  const originalFetch = globalThis.fetch
  try {
    globalThis.fetch = async (_url, { signal }) => new Promise((_, reject) => {
      signal.addEventListener('abort', () => reject(signal.reason), { once: true })
    })
    const server = { pid: process.pid, port: 1234, agentKey: 'a'.repeat(64), build: identity }
    const cancel = new AbortController()
    const pending = serverRequest(server, '/api/agent/call', { name: 'run_status', arguments: { wait_seconds: 300 } }, cancel.signal)
    const cancelled = new Error('caller cancelled')
    cancel.abort(cancelled)
    await assert.rejects(pending, error => error === cancelled)
  } finally { globalThis.fetch = originalFetch }
  fs.appendFileSync(bundle, '\n// rebuilt')
  assert.notEqual(serverBuild(bundle), identity)
  console.log('host startup and discovery: passed')
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
