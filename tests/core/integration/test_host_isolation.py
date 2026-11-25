"""A sandboxed Card on a host tsugumid -- bubblewrap on Linux (CI), Seatbelt
on a Mac (the Web UI) -- reaches nothing on this host: no loopback port, no
file outside its own areas, no write outside its output. These run without
the shared VM, so CI checks the isolation; the VM's own (test_cards_shared_vm.py,
test_workers.py) needs TSUGUMI_TEST_SHARED_VM=1.

Writes are judged by what happened on the host. Seatbelt refuses them, while
under bubblewrap the Card's root is a private tmpfs: the same path is an empty
folder there, a write succeeds and is gone with the sandbox."""

from __future__ import annotations

import json
import os
import socket

from projects import card, conn, run_and_wait, start_run, write_nix_project as write
from harness.real_cards import TXT, accept_in_background, python_card, txt_main
from tsugumi_client import RpcFailure, path, text
from Tsugumi.Data.RunState import RunState


def log_of(c, run_id: str, card_id: str, stream: int = 1) -> str:
    return text(c.call("LogRangeRequest", runId=run_id, card=path(card_id), stream=stream, limitBytes=65536).text)


def test_a_sandboxed_card_reaches_no_port_and_no_file_of_this_host(core_factory, tmp_path):
    """A sandboxed card reaches no port and no file of this host."""
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        accepted = accept_in_background(listener)
        secret = tmp_path / "secret.txt"
        secret.write_text("of this host")
        escape = tmp_path / "escape.txt"
        project_file = tmp_path / "p" / "tsugumi.json"
        probe = python_card(txt_main(f"""
    import socket
    lines = []
    try:
        socket.create_connection(("127.0.0.1", {port}), timeout=5).close()
        lines.append("loopback: connected")
    except OSError:
        lines.append("loopback: refused")
    for label, name in (("read", {str(secret)!r}), ("project read", {str(project_file)!r})):
        try:
            print(open(name).read(), flush=True)
            lines.append(label + ": yes")
        except OSError:
            lines.append(label + ": no")
    for name, mode, value in (({str(escape)!r}, "w", "x"), ({str(secret)!r}, "w", "overwritten"),
                              ({str(project_file)!r}, "a", "tampered")):
        try:
            with open(name, mode) as f:
                f.write(value)
        except OSError:
            pass
    print("\\n".join(lines), flush=True)
    RESULT = 'done'"""), outputs=TXT)
        root = tmp_path / "p"
        write(root, {"start": {"type": "trigger"}, "probe": dict(probe, inputs=[{"name": "start"}])},
              [card("start", "start"), card("probe", "probe")], [conn("start", "out", "probe", "start")])
        d = core_factory(project_root=root)
        c = d.connect()
        run_id, st = run_and_wait(c, "start")
        log = log_of(c, run_id, "probe")
        record = c.call("RunRecordRequest", runId=run_id, card=path("probe")).status
        why = record.failure and text(record.failure.diagnostic)
        assert st.state == RunState.succeeded, (why, log, log_of(c, run_id, "probe", stream=2))
        assert "loopback: refused" in log and not accepted, log
        assert "read: no" in log and "of this host" not in log, log
        assert "project read: no" in log, log
        assert not escape.exists(), log
        assert secret.read_text() == "of this host", log
        assert "tampered" not in project_file.read_text(), log


def test_linux_switches_and_named_grants(core_factory, tmp_path):
    import sys
    import pytest

    if sys.platform != "linux":
        pytest.skip("Linux namespaces")
    shared = tmp_path / "shared"
    shared.mkdir()
    (shared / "data").write_text("readable")
    writable = tmp_path / "write-grant"
    writable.mkdir()
    outside = tmp_path / "outside"
    outside.write_text("outside")
    original_pid = os.readlink("/proc/self/ns/pid")
    original_ipc = os.readlink("/proc/self/ns/ipc")
    scripts = {
        "disabled": python_card(txt_main(f"""
    import os
    assert os.getuid() == {os.getuid()}
    assert os.readlink("/proc/self/ns/pid") == {original_pid!r}
    assert os.readlink("/proc/self/ns/ipc") == {original_ipc!r}
    assert os.path.isfile({str(outside)!r})
    RESULT = 'open'"""), outputs=TXT, sandbox={"filesystem": False, "pid": False, "ipc": False, "identity": False}),
        "enabled": python_card(txt_main(f"""
    import os
    assert os.getuid() == 65534
    assert os.readlink("/proc/self/ns/pid") != {original_pid!r}
    assert os.readlink("/proc/self/ns/ipc") != {original_ipc!r}
    assert not os.path.isfile({str(outside)!r})
    assert os.environ["TSUGUMI_GRANT_SAMPLE_DATA"] == "/host/sample_data"
    assert os.environ["TSUGUMI_GRANT_RESULT"] == "/host/result"
    assert sorted(os.listdir("/host")) == ["result", "sample_data"]
    sample = os.environ["TSUGUMI_GRANT_SAMPLE_DATA"] + "/data"
    assert open(sample).read() == "readable"
    try:
        open(sample, "a").write("modified")
    except OSError:
        pass
    else:
        raise AssertionError("the read grant was writable")
    with open(os.environ["TSUGUMI_GRANT_RESULT"] + "/data", "w") as f:
        f.write("staged\\n")
    RESULT = 'isolated'"""), outputs=TXT, grants={"hostPaths": [
                {"name": "sample_data", "write": False},
                {"name": "result", "write": True},
            ]}),
    }
    root = tmp_path / "p"
    write(root, scripts, [card(name, name) for name in scripts], [],
          host_paths=("sample_data", "result"))
    daemon = core_factory(project_root=root)
    client = daemon.connect()
    with pytest.raises(RpcFailure, match="Host path 'sample_data' is unbound") as failure:
        start_run(client, "enabled")
    assert failure.value.args_map["host_path_name"] == "sample_data"
    assert not client.call("RunHistoryRequest").runs
    (daemon.control / "host-path-bindings.json").write_text(json.dumps({
        "sample_data": {"path": str(shared.resolve()), "write": True},
        "result": {"path": str(writable.resolve()), "write": True},
    }))
    for name in scripts:
        run_id, status = run_and_wait(client, name)
        assert status.state == RunState.succeeded, log_of(client, run_id, name, 2)
    assert (shared / "data").read_text() == "readable"
    assert (writable / "data").read_text() == "staged\n"


def test_linux_wayland_socket_is_visible_only_when_granted(core_factory, tmp_path, monkeypatch):
    import os
    import sys
    import tempfile
    from pathlib import Path

    import pytest

    if sys.platform != "linux":
        pytest.skip("native Linux GUI grant")
    with tempfile.TemporaryDirectory(dir=os.environ.get("TSUGUMI_SOCKET_DIR")) as directory:
        endpoint = str(Path(directory) / "w")
        with socket.socket(socket.AF_UNIX) as compositor:
            compositor.bind(endpoint)
            monkeypatch.setenv("WAYLAND_DISPLAY", endpoint)
            definitions = {
                "granted": python_card(txt_main("""
    import os, stat
    assert stat.S_ISSOCK(os.stat(os.environ["WAYLAND_DISPLAY"]).st_mode)
    RESULT = 'done'"""), outputs=TXT, grants={"gui": True}),
                "closed": python_card(txt_main(f"""
    import os
    assert not os.environ.get("WAYLAND_DISPLAY")
    assert not os.path.exists({endpoint!r})
    RESULT = 'done'"""), outputs=TXT),
            }
            root = tmp_path / "p"
            write(root, definitions, [card(name, name) for name in definitions], [])
            client = core_factory(project_root=root).connect()
            local = client.call("ProjectInspectRequest").executionServices[0]
            assert local.sandbox.gui
            for name in definitions:
                run_id, status = run_and_wait(client, name)
                assert status.state == RunState.succeeded, log_of(client, run_id, name, 2)
