// One startup path for the command and the plugin's detached host. This lock
// covers discovery, build checks, listening and publication of server.json.
import fs from 'node:fs'
import path from 'node:path'
import { randomUUID } from 'node:crypto'

const delay = () => new Promise<void>(resolve => setTimeout(resolve, 50))
const code = (error: unknown) => (error as NodeJS.ErrnoException).code

export async function startupLock(data: string): Promise<() => void> {
  fs.mkdirSync(data, { recursive: true })
  const lock = path.join(data, 'startup.lock')
  // A symlink records ownership in the same atomic operation that acquires it.
  const owner = `${process.pid}-${randomUUID()}`
  const deadline = Date.now() + 30_000
  const read = () => { try { return fs.readlinkSync(lock) } catch (e) { if (code(e) === 'ENOENT') return ''; throw e } }
  while (true) {
    try {
      fs.symlinkSync(owner, lock)
      let released = false
      const release = () => {
        if (released) return
        released = true
        if (read() === owner) fs.unlinkSync(lock)
        process.removeListener('exit', release)
      }
      process.once('exit', release)
      return release
    } catch (e) { if (code(e) !== 'EEXIST') throw e }
    const previous = read()
    if (/^\d+-[a-f0-9-]+$/.test(previous)) {
      let dead = false
      try { process.kill(Number(previous.split('-')[0]), 0) } catch (e) { dead = code(e) === 'ESRCH' }
      if (dead) {
        // Only one waiter can reclaim this owner. Recheck inside the guard:
        // a delayed waiter must never unlink the next owner's lock.
        const guard = path.join(data, `.startup-reap-${previous}`)
        try {
          fs.mkdirSync(guard)
          try { if (read() === previous) fs.unlinkSync(lock) } finally { fs.rmdirSync(guard) }
        } catch (e) { if (code(e) !== 'EEXIST') throw e }
      }
    }
    if (Date.now() >= deadline) throw new Error(`Timed out waiting for Tsugumi startup: ${lock}`)
    await delay()
  }
}
