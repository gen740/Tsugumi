#!/usr/bin/env python3
"""Run tests in short private storage, stop their services, and detect escapes.

Use directly with CMake builds, or through the installed tsugumi-test-env.
Only directory entry names in shared roots are inspected; escaped data is
reported, never removed. No application support directory is inspected.
"""
from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def shared_entries(temporary_roots=None, scratch_root=None):
    uid = os.getuid()
    roots = temporary_roots or {Path('/tmp').resolve(), Path('/var/tmp').resolve(),
                                Path(tempfile.gettempdir()).resolve()}
    scratch = scratch_root or Path('/var/tmp').resolve() / f'tsugumi-scratch-{uid}'
    entries = set()
    for root in roots:
        if not root.exists():
            continue
        with os.scandir(root) as children:
            for entry in children:
                try:
                    if entry.stat(follow_symlinks=False).st_uid == uid:
                        entries.add(str(root / entry.name))
                except FileNotFoundError:
                    pass
                except PermissionError:
                    # A protected entry can still be compared by name. Do not
                    # open it or weaken the check because metadata is denied.
                    entries.add(str(root / entry.name))
    # Listing the common root is sufficient: never traverse project folders.
    if scratch.exists() and not scratch.is_symlink():
        entries.update(str(scratch / name) for name in os.listdir(scratch))
    return entries


def processes():
    result = subprocess.run(['ps', '-axww', '-o', 'pid=,stat=,command='], check=True,
                            stdout=subprocess.PIPE, text=True)
    return {int(pid): command for line in result.stdout.splitlines()
            if len(parts := line.strip().split(None, 2)) == 3
            for pid, state, command in [parts] if not state.startswith("Z")}


def marked_service_process(pid, root):
    # MCP detaches server.js before it writes discovery, with no data-root
    # argument. Pipe-driven fixture helpers also have no root argument.
    # Inspect only known service/helper candidates; never print their environment.
    result = subprocess.run(['ps', 'eww', '-p', str(pid), '-o', 'command='],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=False)
    return f'TSUGUMI_TEST_RUN_ROOT={root}' in result.stdout.split()


def stop_services(root, group=None):
    # Detached UI hosts deliberately survive their UI. Read only this run's
    # discovery records, including hosts whose command has no state argument.
    recorded = set()
    for directory, _, files in os.walk(root, followlinks=False):
        for name in ('agent.ready', 'server.ready', 'server.json'):
            if name in files and not (Path(directory) / name).is_symlink():
                try:
                    recorded.add(int(json.loads((Path(directory) / name).read_text())['pid']))
                except (OSError, ValueError, KeyError, TypeError):
                    pass
    own = str(root) + '/'
    def owned():
        return {pid for pid, command in processes().items()
                if pid != os.getpid() and (own in command or
                    (pid in recorded and any(binary in command for binary in
                     ('tsugumi-agentd', 'tsugumid', 'server.js', 'tsugumi-vmctl', 'tsugumi-vmhost'))) or
                    (any(name in command for name in ('server.js', 'mcp.js', 'model_fixture.py',
                        'test-binary-cache.py', 'tsugumi_document_edit_driver')) and marked_service_process(pid, root)))}
    pending = set()
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if group is not None:
            try:
                os.killpg(group, sig)
            except ProcessLookupError:
                pass
        signalled = set()
        deadline = time.monotonic() + 5
        while True:
            pending = owned()
            if not pending:
                return
            for pid in pending - signalled:
                try:
                    os.kill(pid, sig)
                except ProcessLookupError:
                    pass
            signalled.update(pending)
            if time.monotonic() >= deadline:
                break
            time.sleep(0.05)
    raise RuntimeError(f'Test services did not stop: {sorted(pending)}')


def remove_private_tree(root):
    def retry(function, path, error):
        if isinstance(error[1], FileNotFoundError):
            return
        # Cards can leave unreadable directories. This tree belongs exclusively
        # to this invocation; never apply permission repair to escaped paths.
        if Path(path).is_symlink():
            raise error[1]
        os.chmod(path, 0o700)
        if function in (os.unlink, os.rmdir):
            function(path)
        else:
            shutil.rmtree(path, onerror=retry)
    shutil.rmtree(root, onerror=retry)


def run(arguments):
    if not arguments:
        print('Usage: tests/run-isolated.py COMMAND [ARGUMENT...]', file=sys.stderr)
        return 2
    before = shared_entries()
    # /var/tmp is disk-backed on Linux; /tmp can be tmpfs. Resolve macOS aliases
    # because invocation scratch rejects symbolic links in every parent.
    root = Path(tempfile.mkdtemp(prefix='tsg-', dir=Path('/tmp' if sys.platform == 'darwin' else '/var/tmp').resolve()))
    env = dict(os.environ)
    # Keep explicit per-client support directories usable; the default service
    # is isolated by SUPPORT_DIR, without forcing all clients onto one state.
    env.pop('TSUGUMI_AGENT_STATE', None)
    for key in ('TMPDIR', 'TMP', 'TEMP', 'TSUGUMI_SOCKET_DIR'):
        env[key] = str(root)
    env.update(TSUGUMI_TEST_RUN_ROOT=str(root),
               TSUGUMI_SUPPORT_DIR=str(root / 'support'),
               TSUGUMI_RUN_SCRATCH_DIR=str(root / 'scratch'),
               XDG_CACHE_HOME=str(root / 'cache'), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_DATA_HOME=str(root / 'data'),
               PYTHONPYCACHEPREFIX=str(root / 'pycache'),
               PYTEST_ADDOPTS=env.get('PYTEST_ADDOPTS', '') + ' -p no:cacheprovider')
    child = None
    status = 1
    previous = {}
    def interrupted(sig, _):
        if child is not None:
            try:
                os.killpg(child.pid, sig)
            except ProcessLookupError:
                pass
    try:
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            previous[sig] = signal.signal(sig, interrupted)
        child = subprocess.Popen(arguments, env=env, start_new_session=True)
        status = child.wait()
    finally:
        try:
            if child is not None:
                stop_services(root, child.pid)
        except (OSError, subprocess.SubprocessError, RuntimeError) as error:
            print(f'Cannot finish test service cleanup in {root}: {error}', file=sys.stderr)
            status = 1
        finally:
            try:
                remove_private_tree(root)
            except OSError as error:
                print(f'Tests left private storage at {root}: {error}', file=sys.stderr)
                status = 1
            for sig, handler in previous.items():
                signal.signal(sig, handler)
        escaped = sorted(shared_entries() - before)
        if escaped:
            # New entries cannot be attributed to this run where other work
            # shares the machine, so only a dedicated one (CI, a Nix build)
            # fails on them; elsewhere they are reported.
            dedicated = bool(os.environ.get('CI') or os.environ.get('NIX_BUILD_TOP'))
            print(('Tests left' if dedicated else 'Appeared during the tests (possibly other work):') +
                  ' files outside their private directory (not deleted):\n  ' +
                  '\n  '.join(escaped), file=sys.stderr)
            if dedicated:
                status = 1
    return status if status >= 0 else 128 - status


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
