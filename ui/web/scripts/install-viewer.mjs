import fs from 'node:fs'
import path from 'node:path'

export function installViewer(source, destination) {
  const script = fs.readFileSync(new URL('../src/viewer/viewer.js', import.meta.url), 'utf8')
  const template = fs.readFileSync(path.join(source, 'viewer.html'), 'utf8')
  const page = template.replace('<script src="viewer.js"></script>', () => `<script>\n${script}</script>`)
  if (page === template || /<\/script/i.test(script)) throw new Error('Cannot inline the trusted viewer script')
  fs.rmSync(destination, { recursive: true, force: true })
  fs.mkdirSync(destination, { recursive: true })
  fs.writeFileSync(path.join(destination, 'viewer.html'), page)
}
