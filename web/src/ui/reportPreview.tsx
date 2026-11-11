/** Report HTML embeds its assets. Never execute report scripts in the application. */
export function staticReportHTML(html: string): string {
  const document = new DOMParser().parseFromString(html, 'text/html')
  document.querySelectorAll('script, base, meta[http-equiv], iframe, object, embed').forEach(e => e.remove())
  document.querySelectorAll('[href]').forEach(e => {
    const href = e.getAttribute('href') ?? ''
    if (!href.startsWith('#') && !(e.tagName === 'LINK' && href.startsWith('data:text/css'))) e.removeAttribute('href')
  })
  const policy = "default-src 'none'; img-src data:; style-src 'unsafe-inline' data:; font-src data:; base-uri 'none'; form-action 'none'"
  return `<meta http-equiv="Content-Security-Policy" content="${policy}">` + document.documentElement.outerHTML
}

export function ReportPreview({ html }: { html: string }) {
  return <iframe title="Report HTML" sandbox="" srcDoc={html} style={{ width: '100%', height: '100%', minHeight: 400, border: 0, background: 'white' }} />
}
