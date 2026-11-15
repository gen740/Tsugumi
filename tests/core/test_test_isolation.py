"""The test runner cleans its own storage and reports, but never removes, escapes."""
import importlib.util
import json
import os
import signal
import subprocess
import sys
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location('isolated_runner', Path(__file__).parents[1] / 'run-isolated.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def test_snapshot_is_shallow_and_does_not_follow_project_links(tmp_path, monkeypatch):
    shared = tmp_path / 'shared'
    shared.mkdir()
    scratch = shared / f'tsugumi-scratch-{os.getuid()}'
    scratch.mkdir()
    private = tmp_path / 'private-project'
    private.mkdir()
    (private / 'secret').write_text('must not be inspected')
    (scratch / 'project').symlink_to(private, target_is_directory=True)
    before = runner.shared_entries([shared], scratch)
    (scratch / 'new-project').mkdir()
    assert runner.shared_entries([shared], scratch) - before == {str(scratch / 'new-project')}
    assert str(private / 'secret') not in before
    assert (private / 'secret').read_text() == 'must not be inspected'


@pytest.mark.parametrize('ending,expected', [('sys.exit(7)', 7), ('os.kill(os.getpid(), 9)', 137)])
def test_failed_or_killed_command_leaves_no_private_storage(tmp_path, monkeypatch, ending, expected):
    # No daemon is started in this test. The process-group cleanup is real;
    # only the machine-wide process inventory is replaced (also sandbox-safe).
    monkeypatch.setattr(runner, 'processes', dict)
    monkeypatch.setattr(runner, 'shared_entries', set)
    monkeypatch.setenv('TSUGUMI_HOST_STATE', '/must-not-use-personal-state')
    result = tmp_path / 'environment.json'
    code = f'''import os,sys,json,pathlib
assert 'TSUGUMI_HOST_STATE' not in os.environ
keys = ['TMPDIR','TSUGUMI_SUPPORT_DIR','TSUGUMI_SOCKET_DIR','TSUGUMI_RUN_SCRATCH_DIR','XDG_CACHE_HOME']
pathlib.Path({str(result)!r}).write_text(json.dumps({{k:os.environ[k] for k in keys}}))
pathlib.Path(os.environ['TSUGUMI_RUN_SCRATCH_DIR']).mkdir()
{ending}
'''
    assert runner.run([sys.executable, '-c', code]) == expected
    env = json.loads(result.read_text())
    root = Path(env['TMPDIR'])
    assert not root.exists()
    assert all(Path(value).is_relative_to(root) for value in env.values())
    # Longest native project socket under this run's broker fits macOS.
    nested = root / 'hs-12345678/support/chat-12345678'
    assert len(str(nested / ('e-' + 'a' * 16) / 'env' / ('b' * 16) / 's').encode()) < 104
    assert len(str(nested / ('v-' + 'a' * 24 + '.sock')).encode()) < 104


def test_escape_fails_and_names_the_path_without_deleting_it(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(runner, 'processes', dict)
    shared = tmp_path / 'shared'
    shared.mkdir()
    scratch = tmp_path / 'absent-scratch'
    snapshot = runner.shared_entries
    monkeypatch.setattr(runner, 'shared_entries', lambda: snapshot([shared], scratch))
    escaped = shared / 'unprefixed-escape'
    monkeypatch.setenv('CI', 'true')
    assert runner.run([sys.executable, '-c', f'from pathlib import Path; Path({str(escaped)!r}).mkdir()']) == 1
    assert str(escaped) in capsys.readouterr().err
    assert escaped.is_dir()


@pytest.mark.parametrize("with_group", [True, False])
@pytest.mark.parametrize("ready,binary", [
    ("server.json", "node /runtime/server.js"),
    ("host.ready", "/runtime/tsugumi-hostd"),
])
def test_cleanup_stops_detached_services_but_leaves_unrelated_processes(tmp_path, monkeypatch, with_group, ready, binary):
    owned = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)
    unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)
    # A detached service can have no root argument: its own discovery file is
    # what establishes ownership. The unrelated service must remain alive.
    (tmp_path / ready).write_text(json.dumps({'pid': owned.pid}))
    def inventory():
        result = {unrelated.pid: 'node /elsewhere/server.js'}
        if owned.poll() is None:
            result[owned.pid] = binary
        return result
    monkeypatch.setattr(runner, 'processes', inventory)
    monkeypatch.setattr(runner, 'marked_service_process', lambda pid, root: False)
    try:
        runner.stop_services(tmp_path, owned.pid if with_group else None)
        assert owned.wait(timeout=5) == -signal.SIGTERM
        assert unrelated.poll() is None
    finally:
        for child in (owned, unrelated):
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)


def test_cleanup_handles_unreadable_card_scratch_without_following_links(tmp_path):
    root = tmp_path / 'owned'
    locked = root / 'run/locked'
    locked.mkdir(parents=True)
    (locked / 'output').write_text('scratch')
    outside = tmp_path / 'outside'
    outside.mkdir()
    (outside / 'keep').write_text('real data')
    (root / 'link').symlink_to(outside, target_is_directory=True)
    locked.chmod(0)
    runner.remove_private_tree(root)
    assert not root.exists()
    assert (outside / 'keep').read_text() == 'real data'


def test_cleanup_includes_a_replacement_started_during_shutdown(tmp_path, monkeypatch):
    children = [subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
                                 start_new_session=True) for _ in range(2)]
    def inventory():
        for child in children:
            if child.poll() is None:
                return {child.pid: f'tsugumi-hostd --state-dir {tmp_path}/state'}
        return {}
    monkeypatch.setattr(runner, 'processes', inventory)
    try:
        runner.stop_services(tmp_path, children[0].pid)
        assert all(child.wait(timeout=5) == -signal.SIGTERM for child in children)
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)


def test_host_startup_failure_removes_its_socket_directory(tmp_path, monkeypatch):
    from harness.host import Host

    sockets = tmp_path / 'sockets'
    sockets.mkdir()
    monkeypatch.setenv('TSUGUMI_SOCKET_DIR', str(sockets))
    monkeypatch.setenv('TSUGUMI_HOSTD', str(tmp_path / 'missing-hostd'))
    with pytest.raises(FileNotFoundError):
        Host(tmp_path / 'state')
    assert list(sockets.iterdir()) == []
    assert not (tmp_path / 'state').exists()


def test_sdk_daemon_startup_failure_removes_its_work_directory(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location('sdk_fixtures', Path(__file__).parents[1] / 'python/conftest.py')
    fixtures = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixtures)
    monkeypatch.setattr(fixtures, 'TSUGUMID', tmp_path / 'missing-tsugumid')
    monkeypatch.setenv('TSUGUMI_SOCKET_DIR', str(tmp_path))
    daemon = fixtures.Daemon(tmp_path / 'project')
    with pytest.raises(FileNotFoundError):
        daemon.start()
    assert not daemon.sock_dir.exists()


def test_detached_web_startup_is_owned_before_discovery(tmp_path, monkeypatch):
    child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)
    monkeypatch.setattr(runner, 'processes', lambda: {child.pid: 'node /runtime/server.js'} if child.poll() is None else {})
    monkeypatch.setattr(runner, 'marked_service_process', lambda pid, root: pid == child.pid and root == tmp_path)
    try:
        runner.stop_services(tmp_path)
        assert child.wait(timeout=5) == -signal.SIGTERM
    finally:
        if child.poll() is None:
            child.kill()
        child.wait(timeout=5)


def test_web_environment_ownership_requires_the_exact_marker(tmp_path, monkeypatch):
    from types import SimpleNamespace

    monkeypatch.setattr(runner.subprocess, 'run', lambda *args, **kwargs: SimpleNamespace(
        stdout=f'node server.js TSUGUMI_TEST_RUN_ROOT={tmp_path} OTHER=private'))
    assert runner.marked_service_process(42, tmp_path)
    assert not runner.marked_service_process(42, tmp_path / 'other')
