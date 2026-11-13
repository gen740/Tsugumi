// The desktop app's settings for the Mac's `tsugumi` command: what both have
// lives where the app keeps it, not in a second copy. The app's connection and
// model choices are in its UserDefaults (read and written through
// tsugumi-vmhost, vm.ts). Recent projects belong to the host-side core. API keys
// are its Keychain items already (chat.ts). What only this server has (the
// guide's state, the chats) stays in its data folder.
import type { VMEnvironmentManager } from './vm'

// The server's settings and the app's keys for them (ChatController.AgentSettings,
// AppPreferences, VersionSummarizer).
const KEYS: Record<string, string> = {
  provider: 'agent.provider',
  model: 'agent.model',
  openAIModel: 'agent.model.openai',
  chatGPTModel: 'agent.model.chatgpt',
  effort: 'agent.effort',
  fastModel: 'agent.fastModel',
  fixturePort: 'agent.fixturePort',
  allowedPermissions: 'agent.permissions.allowed',
  webSearch: 'agent.webSearch',
  versionSummaries: 'versions.summaries',
  language: 'app.language',
}
const ALL_KEYS = Object.values(KEYS)

/** The server's settings as the app's defaults. */
function asDefaults(settings: object): Record<string, unknown> {
  const s = settings as Record<string, any>
  const out: Record<string, unknown> = {}
  for (const [name, key] of Object.entries(KEYS)) if (s[name] !== undefined) out[key] = s[name]
  return out
}

export interface DesktopSettings {
  /** What the app has set, in the server's terms. */
  values: Record<string, unknown>
  refresh(): Promise<void>
  /** Writes what changed: a setting left alone stays unset, at the app's own default. */
  save(before: object, after: object): void
}

export function desktopSettings(vm: VMEnvironmentManager): DesktopSettings {
  let raw: Record<string, unknown> = {}
  const apply = () => {
    const values: Record<string, unknown> = {}
    for (const [name, key] of Object.entries(KEYS)) if (raw[key] !== undefined) values[name] = raw[key]
    d.values = values
  }
  const d: DesktopSettings = {
    values: {},
    async refresh() {
      raw = await vm.prefs(ALL_KEYS).catch(() => raw)
      apply()
    },
    save(before, after) {
      const was = asDefaults(before), now = asDefaults(after)
      const changed = Object.fromEntries(Object.entries(now).filter(([k, v]) => JSON.stringify(v) !== JSON.stringify(was[k])))
      const permissions = changed[KEYS.allowedPermissions] as string[] | undefined
      if (permissions) {
        // Kinds only the app knows (the server's settings filter them out) stay.
        const known = new Set(['network', 'sandbox_off', 'host_folder_read', 'host_folder_write'])
        changed[KEYS.allowedPermissions] = [...permissions, ...((raw[KEYS.allowedPermissions] as string[] | undefined) ?? []).filter((k) => !known.has(k))].sort()
      }
      if (!Object.keys(changed).length) return
      Object.assign(raw, changed)
      apply()
      void vm.setPrefs(changed).catch(() => {})
    },
  }
  return d
}
