// Build the default server adapter exactly as the behavioral suite does. The
// child_process double records and refuses every OS command, even on Linux.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import * as esbuild from 'esbuild'

const web = fileURLToPath(new URL('../', import.meta.url))
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-secret-store-'))
try {
  const out = path.join(dir, 'keys.mjs')
  await esbuild.build({
    stdin: { contents: `
      import assert from 'node:assert/strict';
      import { commands } from 'node:child_process';
      import { CoreHost } from './server/coreHost';
      import { ProviderKeys } from './server/providerKeys';
      const call = async () => ({ storage: 'keychain' });
      // CoreHost's own adapter, with no injected runner or platform override.
      const host = new CoreHost('/unused', '/unused', {});
      host.user.call = call;
      await host.keys.provision('anthropic');
      await host.keys.source('openai', 'desktop');
      await host.keys.delete('anthropic', 'desktop');
      // Exercise the macOS path on every CI platform, still using the default runner.
      const keys = new ProviderKeys(call, 'fixture', 'darwin', undefined, {});
      for (const id of ['anthropic', 'openai']) {
        await keys.provision(id);
        await keys.source(id, 'desktop');
        await assert.rejects(keys.save(id, 'FAKE_KEY', 'desktop'), /Keychain/);
        await keys.delete(id, 'desktop');
      }
      console.log(JSON.stringify(commands));
      assert.deepEqual(commands, [], 'default key adapter must not execute security');
    `, resolveDir: web, sourcefile: 'secret-store-guard.ts', loader: 'ts' },
    outfile: out, bundle: true, platform: 'node', format: 'esm', target: 'node22',
    plugins: [{ name: 'refuse-os-commands', setup(build) {
      build.onResolve({ filter: /^node:child_process$/ }, () => ({ path: 'commands', namespace: 'fake' }))
      build.onLoad({ filter: /.*/, namespace: 'fake' }, () => ({ contents: `
        export const commands = [];
        export function execFile(command, args) {
          commands.push({ command, args });
          throw new Error('OS commands forbidden by the test');
        }
        export const spawn = execFile;
      ` }))
    } }],
  })
  for (const marker of ['switch', 'wrapper']) {
    const env = { ...process.env, TSUGUMI_TEST_NO_SECRET_STORE: marker === 'switch' ? '1' : '0' }
    if (marker === 'switch') delete env.TSUGUMI_TEST_RUN_ROOT
    else env.TSUGUMI_TEST_RUN_ROOT = dir
    const recorded = execFileSync(process.execPath, [out], { env, encoding: 'utf8' }).trim()
    assert.deepEqual(JSON.parse(recorded), [])
    console.log(`Secret store guard (${marker}): recorded OS commands ${recorded}`)
  }
} finally {
  fs.rmSync(dir, { recursive: true, force: true })
}
