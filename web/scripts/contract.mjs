// Agent behavior is verified through its public RPC in the core test suite.
import { spawnSync } from 'node:child_process'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..')
const result = spawnSync('ctest', ['--test-dir', process.env.TSUGUMI_BUILD_DIR ?? path.join(repo, 'build'),
  '-R', '^core_agent\\.', '--output-on-failure', '--no-tests=error'], { stdio: 'inherit' })
if (result.error) throw result.error
process.exit(result.status ?? 1)
