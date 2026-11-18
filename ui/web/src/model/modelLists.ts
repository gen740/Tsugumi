// The chat's model menu lists every connection again when it opens. One
// connection's failure or slowness touches nothing else: its saved list stays
// (the server saves only what it got), and opening the menu again does not
// ask a connection that has not answered yet.
export class ModelListRefresher {
  private running = new Set<string>()

  constructor(private readonly fetchList: (id: string) => Promise<unknown>) {}

  /** `finished` is called for each connection, as soon as its answer comes. */
  refresh(ids: string[], finished: (id: string, ok: boolean) => void) {
    for (const id of ids) {
      if (this.running.has(id)) continue
      this.running.add(id)
      const done = (ok: boolean) => {
        this.running.delete(id)
        finished(id, ok)
      }
      this.fetchList(id).then(() => done(true), () => done(false))
    }
  }
}
