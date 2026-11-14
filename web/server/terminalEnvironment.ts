import os from 'node:os'

/** Build a terminal environment from runtime needs, never the Web host's environment. */
export function terminalEnvironment(project: string): Record<string, string> {
  const user = os.userInfo()
  return {
    PATH: `${user.homedir}/.nix-profile/bin:/nix/var/nix/profiles/default/bin:/run/current-system/sw/bin:/usr/bin:/bin:/usr/sbin:/sbin`,
    HOME: user.homedir, USER: user.username, LOGNAME: user.username,
    SHELL: user.shell || '/bin/bash', TMPDIR: '/tmp', LANG: 'C.UTF-8',
    TERM: 'xterm-256color', COLORTERM: 'truecolor', TSUGUMI_PROJECT: project,
  }
}
