#!/usr/bin/env bash
# A disposable localhost SSH service for the hosted Linux integration job.
set -euo pipefail

if [[ ${GITHUB_ACTIONS:-} != true || ${RUNNER_OS:-} != Linux ]]; then
  echo "Run this setup on a disposable GitHub Actions Linux runner." >&2
  exit 2
fi

scratch=$(mktemp -d "$HOME/.tsugumi-ssh-ci.XXXXXX")
username=$(id -un)
uid=$(id -u)
sshd=/usr/sbin/sshd
enabled_linger=false

cleanup() {
  if [[ -f "$scratch/sshd.pid" ]]; then
    sudo kill "$(cat "$scratch/sshd.pid")" || true
  fi
  if [[ $enabled_linger == true ]]; then
    sudo loginctl disable-linger "$username" || true
  fi
  if [[ -d $scratch/state ]]; then
    find "$scratch/state" -type d -exec chmod u+w {} +
  fi
  rm -rf "$scratch"
}
trap cleanup EXIT

if [[ ! -x $sshd ]]; then
  sudo apt-get update
  sudo apt-get install -y openssh-server
fi
sudo install -d -m 0755 /run/sshd
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0

if [[ $(loginctl show-user "$username" --property=Linger --value || true) != yes ]]; then
  sudo loginctl enable-linger "$username"
  enabled_linger=true
fi
sudo systemctl start "user@$uid.service"

port=$(python3 - <<'PY'
import socket

with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    print(listener.getsockname()[1])
PY
)
ssh-keygen -q -t ed25519 -N '' -f "$scratch/host"
ssh-keygen -q -t ed25519 -N '' -f "$scratch/client"
printf '[127.0.0.1]:%s %s\n' "$port" "$(cut -d ' ' -f 1,2 "$scratch/host.pub")" > "$scratch/known_hosts"
chmod 600 "$scratch/client.pub"

cat > "$scratch/sshd_config" <<EOF
Port $port
ListenAddress 127.0.0.1
HostKey $scratch/host
AuthorizedKeysFile $scratch/client.pub
AllowUsers $username
PidFile $scratch/sshd.pid
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin no
AllowAgentForwarding no
AllowTcpForwarding no
X11Forwarding no
UsePAM yes
EOF
cat > "$scratch/ssh_config" <<EOF
Host tsugumi-ci-executor
  HostName 127.0.0.1
  Port $port
  User $username
  IdentityFile $scratch/client
  IdentitiesOnly yes
  UserKnownHostsFile $scratch/known_hosts
  StrictHostKeyChecking yes
  BatchMode yes
EOF
sudo "$sshd" -t -f "$scratch/sshd_config"
sudo "$sshd" -f "$scratch/sshd_config" -E "$scratch/sshd.log"

# The product still uses OpenSSH. The wrapper selects the test-only client
# configuration without changing the runner account's normal SSH setup.
mkdir "$scratch/bin"
printf '#!/usr/bin/env bash\nexec %q -F %q "$@"\n' "$(command -v ssh)" "$scratch/ssh_config" > "$scratch/bin/ssh"
chmod 700 "$scratch/bin/ssh"
"$scratch/bin/ssh" tsugumi-ci-executor \
  'test -f /sys/fs/cgroup/cgroup.controllers &&
   systemd-run --user --quiet --wait --collect -p CPUQuota=50% -p MemoryMax=64M /usr/bin/true'

nix build .#remote-gateway --out-link "$scratch/gateway"
printf '#!/usr/bin/env bash\nexport TSUGUMI_REMOTE_STATE_DIR=%q\nexec %q "$@"\n' \
  "$scratch/state" "$scratch/gateway/bin/tsugumi-remote-gateway" > "$scratch/run-gateway"
chmod 700 "$scratch/run-gateway"

PATH="$scratch/bin:$PATH" \
  TSUGUMI_REMOTE_TEST_SSH_HOST=tsugumi-ci-executor \
  TSUGUMI_REMOTE_TEST_GATEWAY="$scratch/run-gateway" \
  "$CORE_TEST_ENV" python3 -m pytest -q -m environment tests/core/integration/test_remote_execution.py
