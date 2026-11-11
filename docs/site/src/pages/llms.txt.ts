import { catalog, introduction, llmsTxt } from '../lib/documents.mjs'

export function GET() {
  const { groups, markdownPages } = catalog()
  return new Response(llmsTxt(groups, markdownPages, introduction().tagline), {
    headers: { 'Content-Type': 'text/plain; charset=utf-8' },
  })
}
