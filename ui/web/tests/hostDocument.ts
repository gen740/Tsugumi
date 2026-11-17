// Socket-free adapter to the production host document implementation.
import { spawnSync } from 'node:child_process'
import path from 'node:path'
export function hostDocumentReply(request: unknown): any {
  const binary = process.env.TSUGUMI_DOCUMENT_EDIT_DRIVER ?? path.join(process.env.TSUGUMI_BUILD_DIR ?? path.join(process.argv[2], 'build'), 'tests/core/tsugumi_document_edit_driver')
  const result = spawnSync(binary, { input: JSON.stringify(request), encoding: 'utf8' })
  if (result.error || result.status !== 0) throw result.error ?? new Error(result.stderr)
  return JSON.parse(result.stdout)
}
