// A presentation snapshot; agentd stores and validates the shared setting.
import { api } from '../api'
import { Observable } from './observable'

export class AgentAppearance extends Observable {
  animateWork = true
  error?: string
  private generation = 0
  private pending?: Promise<void>

  constructor(private readonly request: (body?: { animateWork: boolean }) => Promise<{ animateWork: boolean }> =
    (body) => api('/api/agent/appearance', body)) { super() }

  async reload() {
    await this.pending
    await this.read()
  }
  async setAnimateWork(animateWork: boolean) {
    const previous = this.pending
    const edit = (async () => {
      await previous
      await this.read({ animateWork })
    })()
    this.pending = edit
    await edit
    if (this.pending === edit) this.pending = undefined
  }

  private async read(body?: { animateWork: boolean }) {
    const generation = ++this.generation
    try {
      const reply = await this.request(body)
      if (generation !== this.generation) return
      this.animateWork = reply.animateWork
      this.error = undefined
    } catch (e) {
      if (generation !== this.generation) return
      this.error = String((e as Error).message ?? e)
    }
    this.changed()
  }
}

export const agentAppearance = new AgentAppearance()
