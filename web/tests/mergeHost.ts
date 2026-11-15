// Client gesture/undo tests use the actual pure host implementation.
import { spawnSync } from 'node:child_process'
import path from 'node:path'
import { ProjectSession } from '../src/model/session'
ProjectSession.prototype.prepareMerge = async function(baseJson, oursJson, theirsJson) {
  const binary = process.env.TSUGUMI_DOCUMENT_EDIT_DRIVER ?? path.join(process.env.TSUGUMI_BUILD_DIR ?? path.join(process.argv[2], 'build'), 'tests/core/tsugumi_document_edit_driver')
  const result = spawnSync(binary, { input: JSON.stringify({ baseJson, oursJson, theirsJson }), encoding: 'utf8' })
  if (result.error || result.status !== 0) throw result.error ?? new Error(result.stderr)
  return JSON.parse(result.stdout)
}
