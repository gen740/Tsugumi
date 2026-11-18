// The actual launcher observes a refused child without binding an RPC socket.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { CoreHost, CoreHostError } from '../server/coreHost'
import type { Environments } from '../server/environment'

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-host-readiness-'))
const previous = { binary: process.env.TSUGUMI_HOSTD, state: process.env.TSUGUMI_HOST_STATE }
try {
  const binary = path.join(directory, 'hostd')
  fs.writeFileSync(binary, '#!/bin/sh\necho "Refused incompatible host state" >&2\nexit 5\n', { mode: 0o700 })
  process.env.TSUGUMI_HOSTD = binary
  const state = path.join(directory, 'host-core')
  process.env.TSUGUMI_HOST_STATE = state
  fs.mkdirSync(state)
  fs.writeFileSync(path.join(state, 'host.sqlite'), 'must stay untouched')
  const host = new CoreHost(directory, directory, {} as Environments)
  await assert.rejects(host.ready(), (error: Error) => {
    assert.ok(error instanceof CoreHostError)
    assert.equal(error.code, 'startup_exit')
    assert.equal(error.path, path.join(state, 'host.log'))
    return true
  })
  assert.equal(fs.readFileSync(path.join(state, 'host.sqlite'), 'utf8'), 'must stay untouched')
  // A missing state folder must be left for the service to create and number.
  process.env.TSUGUMI_HOST_STATE = path.join(directory, 'fresh')
  fs.writeFileSync(binary, '#!/bin/sh\nif [ -e "$2" ]; then exit 1; fi\nexit 5\n', { mode: 0o700 })
  await assert.rejects(new CoreHost(directory, directory, {} as Environments).ready(), (error: CoreHostError) => error.code === 'startup_exit')
  assert.ok(!fs.existsSync(process.env.TSUGUMI_HOST_STATE))
  // Successful first startup also leaves the log at its persistent host path.
  fs.writeFileSync(binary, `#!${process.execPath}
const fs = require('node:fs'), path = require('node:path');
const state = process.argv[process.argv.indexOf('--state-dir') + 1];
if (fs.existsSync(state)) process.exit(1);
fs.mkdirSync(state);
fs.writeSync(2, 'fresh host startup\\n');
const ready = JSON.stringify({ protocol: 1, pid: process.pid,
  unix_socket: 'fixture-agent', host_socket: 'fixture-host', user_token: 'fixture', external_token: 'fixture' });
fs.writeFileSync(path.join(state, 'host.ready'), ready);
process.stdout.write(ready + '\\n');
// Keep the fake host alive until the test sends TERM.
const { port1 } = new (require('node:worker_threads').MessageChannel)();
port1.on('message', () => {});
`, { mode: 0o700 })
  const ready = await new CoreHost(directory, directory, {} as Environments).ready()
  try {
    assert.match(fs.readFileSync(path.join(process.env.TSUGUMI_HOST_STATE, 'host.log'), 'utf8'), /fresh host startup/)
  } finally { process.kill(ready.pid, 'SIGTERM') }
  console.log('host startup readiness and recovery: passed')
} finally {
  if (previous.binary === undefined) delete process.env.TSUGUMI_HOSTD
  else process.env.TSUGUMI_HOSTD = previous.binary
  if (previous.state === undefined) delete process.env.TSUGUMI_HOST_STATE
  else process.env.TSUGUMI_HOST_STATE = previous.state
  fs.rmSync(directory, { recursive: true, force: true })
}
