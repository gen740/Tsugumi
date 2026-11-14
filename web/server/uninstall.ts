// Remove only files owned by cli/install.sh; user data and Nix stay in place.
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const stat = (file: string) => {
  try { return fs.lstatSync(file) } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return undefined
    throw error
  }
}

export function uninstall(args: string[]): number {
  try {
    if (args.some(a => a !== '--yes') || args.length > 1) {
      throw new Error('Usage: tsugumi uninstall [--yes]')
    }
    const home = os.homedir()
    const local = path.join(home, '.local')
    const share = path.join(local, 'share')
    const bin = path.join(local, 'bin')
    const data = path.join(share, 'tsugumi')
    const command = path.join(bin, 'tsugumi')
    const current = path.join(data, 'current')
    // Never follow a redirected install root or launcher folder when deleting.
    for (const dir of [local, share, bin, data]) {
      const s = stat(dir)
      if (s && (!s.isDirectory() || s.isSymbolicLink())) throw new Error(`Refusing to remove a redirected installation: ${dir}`)
    }
    const launcher = stat(command)
    if (launcher && (!launcher.isSymbolicLink() || path.resolve(bin, fs.readlinkSync(command)) !== path.join(current, 'tsugumi'))) {
      throw new Error(`Not an installer-owned launcher: ${command}`)
    }
    const link = stat(current)
    const targets: string[] = []
    if (link) {
      if (!link.isSymbolicLink()) throw new Error(`Not an installer-owned version link: ${current}`)
      const version = fs.readlinkSync(current)
      if (!/^[A-Za-z0-9][A-Za-z0-9._+-]*$/.test(version)) throw new Error(`Invalid installed version: ${version}`)
      const release = path.join(data, version)
      const releaseStat = stat(release)
      const marker = path.join(release, '.installed-by-tsugumi')
      const markerStat = stat(marker)
      if (!releaseStat?.isDirectory() || releaseStat.isSymbolicLink() || !markerStat?.isFile() || markerStat.isSymbolicLink()
        || fs.readFileSync(marker, 'utf8').trim() !== version) {
        throw new Error(`Not an installer-owned release: ${release}`)
      }
      targets.push(release, current)
    }
    // Old immutable generations stay available during updates. The user stops
    // running hosts before explicitly uninstalling them; foreign entries stay.
    if (stat(data)) for (const name of fs.readdirSync(data)) {
      const release = path.join(data, name)
      if (targets.includes(release)) continue
      const marker = path.join(release, '.installed-by-tsugumi')
      const r = stat(release), m = r?.isDirectory() && !r.isSymbolicLink() ? stat(marker) : undefined
      if (m?.isFile() && !m.isSymbolicLink() && fs.readFileSync(marker, 'utf8').trim() === name) targets.push(release)
    }
    if (launcher) targets.push(command)
    if (!targets.length) {
      console.log('No installer-managed tsugumi command is installed.')
      return 0
    }
    console.log('Remove the tsugumi command and its installed runtime:')
    targets.forEach(target => console.log(`  ${target}`))
    console.log('Projects, settings, the shared Mac VM, the desktop app, Nix and its store are kept.')
    if (!args.includes('--yes')) {
      console.log('Quit Tsugumi and stop any running tsugumi server, then run: tsugumi uninstall --yes')
      return 0
    }
    for (const target of targets) fs.rmSync(target, { recursive: true, force: true })
    // Extra files here do not belong to this installation and must survive.
    if (stat(data) && fs.readdirSync(data).length === 0) fs.rmdirSync(data)
    console.log('The tsugumi command has been uninstalled.')
    return 0
  } catch (error) {
    console.error(`tsugumi: ${error instanceof Error ? error.message : String(error)}`)
    return 1
  }
}
