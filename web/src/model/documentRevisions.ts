import type { DocumentDeltaT } from '../gen/tsugumi/rpc/document-delta'

/** Replies and notifications feed the same revision order. No echo is applied twice. */
export class DocumentRevisions {
  private pending = new Map<bigint, { delta: DocumentDeltaT; fromSave: boolean }>()
  private waiting: { revision: bigint; resolve: () => void; reject: (error: Error) => void }[] = []
  constructor(public revision = 0n) {}

  reset(revision: bigint) {
    this.revision = revision
    for (const [base, { delta }] of this.pending) if (delta.rev! <= revision) this.pending.delete(base)
    this.finish()
  }

  receive(delta: DocumentDeltaT, apply: (delta: DocumentDeltaT, fromSave: boolean) => void, fromSave = false) {
    if (delta.rev == null || delta.baseRev == null || delta.rev <= this.revision) return
    this.pending.set(delta.baseRev, { delta, fromSave })
    this.drain(apply)
  }

  drain(apply: (delta: DocumentDeltaT, fromSave: boolean) => void) {
    for (;;) {
      const next = this.pending.get(this.revision)
      if (!next) break
      this.pending.delete(this.revision)
      apply(next.delta, next.fromSave)
      this.revision = next.delta.rev!
      this.finish()
    }
  }

  applied(revision: bigint): Promise<void> {
    if (this.revision >= revision) return Promise.resolve()
    return new Promise((resolve, reject) => this.waiting.push({ revision, resolve, reject }))
  }

  disconnect(error: Error) {
    this.pending.clear()
    for (const waiter of this.waiting) waiter.reject(error)
    this.waiting = []
  }

  private finish() {
    const done = this.waiting.filter(w => w.revision <= this.revision)
    this.waiting = this.waiting.filter(w => w.revision > this.revision)
    for (const waiter of done) waiter.resolve()
  }
}
