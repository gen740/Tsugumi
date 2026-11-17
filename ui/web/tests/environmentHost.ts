import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import * as fb from 'flatbuffers'
import { EnvironmentManager, type HostEvent } from '../server/environment'
import { EnvironmentAction } from '../shared/gen/tsugumi/host/environment-action'
import { EnvironmentPhase } from '../shared/gen/tsugumi/host/environment-phase'
import { EnvironmentStateT } from '../shared/gen/tsugumi/host/environment-state'
import { EventT } from '../shared/gen/tsugumi/host/event'
import { EventPayload } from '../shared/gen/tsugumi/host/event-payload'
import { Message, MessageT } from '../shared/gen/tsugumi/host/message'
import { Payload } from '../shared/gen/tsugumi/host/payload'
import { decodeReply, encodeRequest } from '../server/serviceWire'

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-environment-adapter-'))
try {
  fs.writeFileSync(path.join(root, 'tsugumi.json'), '{}')
  const calls: { method: string; fields: any }[] = []
  let receive: (event: HostEvent) => void = () => {}
  let failure = false
  const manager = new EnvironmentManager({ tsugumid: '/daemon', worker: '/worker', sdkDir: '/sdk' }, root,
    () => ['http://localhost:8780'], async <T>(method: string, fields: any): Promise<T> => {
      calls.push({ method, fields })
      const state = { project: root, key: manager.list()[0].key,
        phase: fields.action === 'Stop' ? EnvironmentPhase.Stopped : failure ? EnvironmentPhase.Failed : EnvironmentPhase.Ready,
        endpoint: 'ws://localhost:1/rpc', token: 't', project_id: 'project', readyFile: '/ready',
        controlDir: '/control', message: 'fixture failure', recovery: 'Retry the virtual machine' }
      return (method === 'AttachFoldersRequest' ? { state, folders: [{ folder: '/one', resource: 'folder:project:one' }] } : state) as T
    }, (handler) => { receive = handler; return () => true })
  assert.equal((await manager.start(root)).state, 'ready')
  assert.deepEqual(calls[0], { method: 'EnvironmentRequest', fields: { project: fs.realpathSync(root), action: 'Start',
    origins: ['http://localhost:8780'], archive_incompatible_history: false } })
  assert.deepEqual(await manager.attachFolders(root, ['/one']), [{ folder: '/one', resource: 'folder:project:one' }])
  assert.deepEqual(calls.at(-1)!.fields, { project: fs.realpathSync(root), folders: ['/one'] })
  await manager.removeFolders(root, ['/one'])
  assert.equal(calls.at(-1)!.fields.action, 'RemoveFolders')
  assert.deepEqual(await manager.attachedFolders(root), [])
  await manager.start(root, true)
  assert.equal(calls.at(-1)!.fields.archive_incompatible_history, true)
  assert.deepEqual(calls.map((call) => call.method), ['EnvironmentRequest', 'AttachFoldersRequest', 'EnvironmentRequest', 'EnvironmentRequest', 'EnvironmentRequest'])
  receive({ type: 'EnvironmentStopped', value: { state: { key: manager.list()[0].key, phase: EnvironmentPhase.Stopped } } })
  assert.equal(manager.list()[0].state.state, 'stopped')
  failure = true
  const failed = await manager.start(root)
  assert.equal(failed.state, 'failed')
  if (failed.state === 'failed') assert.equal(failed.recovery, 'Retry the virtual machine')
  const encoded = encodeRequest('EnvironmentRequest', { project: fs.realpathSync(root), action: 'Start' }, 1n, true)
  const request = Message.getRootAsMessage(new fb.ByteBuffer(encoded)).unpack().payload as any
  assert.equal(request.value.action, EnvironmentAction.Start)
  assert.equal(request.value.project, fs.realpathSync(root))
  assert.deepEqual(request.value.folders, [])
  assert.deepEqual(request.value.origins, [])
  const attach = encodeRequest('AttachFoldersRequest', { project: fs.realpathSync(root), folders: [] }, 2n, true)
  const attachment = Message.getRootAsMessage(new fb.ByteBuffer(attach)).unpack().payload as any
  assert.equal(attachment.value.project, fs.realpathSync(root))
  assert.deepEqual(attachment.value.folders, [], 'An omitted vector is an empty batch, never a malformed frame')
  // Direct endpoint registration must also carry the host folder for project conversations.
  const registration = encodeRequest('RegisterProjectRequest', { project_id: 'project',
    endpoint: 'ws://localhost:1/rpc', token: 't', title: 'Project', path: fs.realpathSync(root) }, 2n, true)
  const registered = Message.getRootAsMessage(new fb.ByteBuffer(registration)).unpack().payload as any
  assert.equal(registered.value.projectId, 'project')
  assert.equal(registered.value.path, fs.realpathSync(root))
  const state = new EnvironmentStateT(); state.project = root; state.key = 'key'; state.phase = EnvironmentPhase.Ready
  const builder = new fb.Builder()
  Message.finishMessageBuffer(builder, new MessageT(0n, Payload.Event, new EventT(EventPayload.EnvironmentState, state)).pack(builder))
  const event = decodeReply(builder.asUint8Array(), true)
  assert.equal(event.event?.type, 'EnvironmentState')
  assert.equal(event.event?.value.phase, EnvironmentPhase.Ready)
  console.log('environment Host adapter: passed')
} finally { fs.rmSync(root, { recursive: true, force: true }) }
