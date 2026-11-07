// What is typed goes to the terminal in order, one request at a time; what is
// typed meanwhile goes together in the next one.
export class TerminalInput {
  private pending = ''
  private sending = false

  constructor(private readonly send: (data: string) => Promise<unknown>, private readonly failed: (e: unknown) => void) {}

  write(data: string) {
    this.pending += data
    if (!this.sending) void this.flush()
  }

  private async flush() {
    this.sending = true
    while (this.pending) {
      const data = this.pending
      this.pending = ''
      try {
        await this.send(data)
      } catch (e) {
        this.failed(e)
      }
    }
    this.sending = false
  }
}
