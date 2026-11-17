// Host-owned structure. Clients only select the language and draw these rows.
export interface ChatRow {
  id: string; kind: string; role: string; text: string; name: string
  arguments: string; result: string; state: string; archived: boolean
  summaryJa: string; summaryEn: string; scopeJa: string; scopeEn: string
  instruction: string; failed: number; recovered: number
  links: { title: string; url: string }[]; children: ChatRow[]
}
export interface AccessGroup {
  title: string; path: string; displayPath: string; scope: string
  lines: { request: string; operation: string; scope: string; neededNow: boolean }[]
}
