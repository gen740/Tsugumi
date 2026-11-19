// Socket-free adapter to the production host document implementation.
import path from 'node:path'
import { MessageChannel, Worker, receiveMessageOnPort } from 'node:worker_threads'

function startDriver() {
  const binary = process.env.TSUGUMI_DOCUMENT_EDIT_DRIVER ?? path.join(process.env.TSUGUMI_BUILD_DIR ?? path.join(process.argv[2], 'build'), 'tests/core/tsugumi_document_edit_driver')
  const { port1, port2 } = new MessageChannel()
  const ready = new Int32Array(new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT))
  // The operation fixture consumes replies synchronously. Keep pipe I/O on a
  // worker so the main thread can wait without blocking stdout delivery.
  const worker = new Worker(`
    const { parentPort, workerData: { binary, port, ready } } = require('node:worker_threads')
    const { spawn } = require('node:child_process')
    const { createInterface } = require('node:readline')
    const child = spawn(binary, { stdio: ['pipe', 'pipe', 'inherit'] })
    let failure, pending
    function reply(message) {
      pending = false
      port.postMessage(message)
      Atomics.store(ready, 0, 1)
      Atomics.notify(ready, 0)
    }
    function fail(error) {
      failure ??= error.message
      if (pending) reply(JSON.stringify({ error: failure }))
    }
    child.on('error', fail)
    child.stdin.on('error', fail)
    child.on('close', (code, signal) => fail(new Error('Document driver exited: ' + (signal ?? code))))
    createInterface({ input: child.stdout }).on('line', reply)
    parentPort.on('message', line => {
      if (line === null) {
        child.stdin.end()
        port.close()
        parentPort.close()
        return
      }
      pending = true
      if (failure) reply(JSON.stringify({ error: failure }))
      else child.stdin.write(line + '\\n')
    })
  `, { eval: true, workerData: { binary, port: port2, ready }, transferList: [port2] })
  worker.unref()
  port1.unref()
  return { worker, port: port1, ready }
}

let driver: ReturnType<typeof startDriver> | undefined
process.once('beforeExit', () => {
  if (!driver) return
  driver.worker.ref()
  driver.worker.postMessage(null)
  driver.port.close()
})

export function hostDocumentReply(request: unknown): any {
  const { worker, port, ready } = driver ??= startDriver()
  Atomics.store(ready, 0, 0)
  worker.postMessage(JSON.stringify(request))
  if (Atomics.wait(ready, 0, 0, 15_000) === 'timed-out') {
    port.close()
    void worker.terminate()
    driver = undefined
    throw new Error('Document driver did not reply')
  }
  const reply = JSON.parse(receiveMessageOnPort(port)!.message)
  if (reply.error !== undefined) throw new Error(reply.error)
  return reply.result
}
