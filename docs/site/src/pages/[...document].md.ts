import { catalog, markdownPage } from '../lib/documents.mjs'

export function getStaticPaths() {
  const { markdownPages } = catalog()
  return [...markdownPages].map(([source, out]) => ({
    params: { document: out.slice(0, -3) }, props: { source, out },
  }))
}

export function GET({ props }) {
  const { markdownPages } = catalog()
  return new Response(markdownPage(props.source, props.out, markdownPages), {
    headers: { 'Content-Type': 'text/markdown; charset=utf-8' },
  })
}
