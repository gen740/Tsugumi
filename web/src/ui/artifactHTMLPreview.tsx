/**
 * Shows the static form of an HTML artifact (model/artifactHTML). The empty sandbox is the
 * boundary: no scripts, forms, popups or same-origin access, whatever the document holds.
 */
export function ArtifactHTMLPreview({ html }: { html: string }) {
  return <iframe title="HTML artifact" sandbox="" referrerPolicy="no-referrer" srcDoc={html} style={{ width: '100%', height: '100%', minHeight: 400, border: 0, background: 'white' }} />
}
