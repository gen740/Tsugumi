import assert from 'node:assert/strict'
import * as A from '../shared/gen/tsugumi/agent'
import { ConversationEvents } from '../src/wire/agentEvents'

const reads: { id: string; seq: bigint; finish: (page: A.EventsPageT) => void }[] = []
const events: unknown[] = []
let wake: (() => void) | undefined
const observer = new ConversationEvents(
  event => { events.push(event) }, error => assert.fail(String(error)),
  (id, seq) => new Promise(finish => reads.push({ id, seq, finish })),
  async () => {},
  (_id, notify) => { wake = notify; return () => { wake = undefined } },
)
const tick = () => new Promise<void>(resolve => setImmediate(resolve))
const reply = (seq: bigint, text: string) => new A.EventsPageT(
  [new A.SequencedEventT(seq, Object.assign(new A.EventT(), {
    kind: 'toolStarted', name: 'guide_show', inputJson: text,
  }))], seq, true,
)
const after = 1n << 56n
observer.start('first', after)
await tick()
assert.deepEqual(reads.map(r => [r.id, r.seq]), [['first', after]])
wake!()
wake!()
reads[0].finish(reply(after + 1n, '{"say":"hello"}'))
await tick()
assert.deepEqual(reads.map(r => r.seq), [after, after + 1n])
reads[1].finish(new A.EventsPageT([], after + 1n))
await tick()
assert.equal((events[0] as { arguments: string }).arguments, '{"say":"hello"}')
assert.equal(reads.length, 2, 'an idle conversation waits for a host notification')
wake!()
await tick()
observer.start('second', 1n)
await tick()
reads[2].finish(reply(after + 2n, 'late first'))
reads[3].finish(reply(2n, 'second'))
await tick()
assert.deepEqual(events.map(e => (e as { arguments: string }).arguments), ['{"say":"hello"}', 'second'])
observer.close()
assert.equal(wake, undefined)
