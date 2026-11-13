// Reads a login keychain item with `security`, which macOS may answer with a
// dialog (an item the app saved trusts the app, not `security`). The secret is
// read once and kept in memory, also when the read was refused, so however
// often a key is needed the dialog comes at most once; reading the item's
// attributes never asks, and when they change (a key saved elsewhere) it is
// read again. No `security` flag reads without the dialog.
export type Run = (cmd: string, args: string[]) => Promise<{ code: number; out: string }>

export function keychainReader(run: Run, service: string) {
  const known = new Map<string, { attributes: string; secret: Promise<string | undefined> }>()
  const attributes = (account: string) => run('/usr/bin/security', ['find-generic-password', '-s', service, '-a', account])
  return {
    /** Whether the item is there; never asks. */
    async exists(account: string) {
      return (await attributes(account)).code === 0
    },
    /** The secret; undefined when there is no item or the read was refused. */
    async read(account: string): Promise<string | undefined> {
      const a = await attributes(account)
      if (a.code !== 0) {
        known.delete(account)
        return undefined
      }
      const k = known.get(account)
      if (k && k.attributes === a.out) return k.secret
      const secret = run('/usr/bin/security', ['find-generic-password', '-s', service, '-a', account, '-w'])
        .then((r) => (r.code === 0 && r.out ? r.out : undefined))
      known.set(account, { attributes: a.out, secret })
      return secret
    },
    /** This server saved or deleted the item (attributes may look the same within a second). */
    forget(account: string) {
      known.delete(account)
    },
  }
}
