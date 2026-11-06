// What a listing of a connection's models saves in the settings.
export interface ModelListSettings {
  models: Partial<Record<string, string[]>>
  modelNames: Record<string, string>
}

/** The settings with a new list, or null for an empty answer, which never
 *  replaces a saved list. Only the ChatGPT account's catalog has display names. */
export function withModelList<S extends ModelListSettings>(s: S, id: string, list: { id: string; label: string }[]): S | null {
  if (!list.length) return null
  const modelNames = id === 'chatgpt' ? Object.fromEntries(list.map((m) => [m.id, m.label])) : s.modelNames
  return { ...s, models: { ...s.models, [id]: list.map((m) => m.id) }, modelNames }
}
