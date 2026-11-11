"""The `tsugumi` command: what cli/install.sh does on each platform (uname and
Nix are stand-ins here, so all of it runs anywhere), and the web server with the
Mac's VM backend against a stand-in for tsugumi-vmhost: the desktop app's
settings and recent projects, with no second copy. A real VM is checked by hand
(docs/Development.md)."""

from __future__ import annotations

import hashlib
import http.client
import json
import os
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tarfile
import textwrap
import time
from pathlib import Path

import pytest

from conftest import WEB_DIST
from test_claude_code_plugin import isolated_environment, stop_resident_services

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "cli" / "install.sh"
SERVER = WEB_DIST / "server.js"
needs_server = pytest.mark.skipif(not shutil.which("node") or not SERVER.exists(), reason="needs node and web/dist (cd web && npm run build)")


def executable(path: Path, text: str):
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


def artifact(release: Path, name: str, version: str, sha: str | None = None):
    """A release file: tsugumi/ with VERSION and a command that prints it."""
    tree = release / "build" / version / "tsugumi"
    tree.mkdir(parents=True)
    (tree / "VERSION").write_text(version + "\n")
    executable(tree / "tsugumi", "#!/bin/sh\ncat \"$(dirname \"$(readlink -f \"$0\")\")/VERSION\"\n")
    with tarfile.open(release / name, "w:gz") as t:
        t.add(tree, "tsugumi")
    digest = sha or hashlib.sha256((release / name).read_bytes()).hexdigest()
    (release / f"{name}.sha256").write_text(f"{digest}  {name}\n")


@pytest.fixture
def machine(tmp_path):
    """A home folder, a release folder and an install() on a PATH whose uname and sw_vers say what it is told."""
    home = tmp_path / "home"
    home.mkdir()
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    release = tmp_path / "release"
    release.mkdir()

    def install(system: str, machine: str, macos: str = "26.0", path_extra: str = "", nix: bool = False, command: list[str] | None = None):
        executable(stubs / "uname", f'#!/bin/sh\n[ "$1" = -s ] && echo {system} || echo {machine}\n')
        executable(stubs / "sw_vers", f"#!/bin/sh\necho {macos}\n")
        if nix:
            # Builds the command from the extracted source, as the flake's cli package does.
            executable(stubs / "nix", textwrap.dedent('''\
                #!/bin/sh
                while [ $# -gt 0 ]; do
                  case "$1" in
                    path:*) src="${1#path:}"; src="${src%#cli}" ;;
                    --out-link) shift; out="$1" ;;
                  esac
                  shift
                done
                mkdir -p "$out/bin"
                printf '#!/bin/sh\\necho built %s\\n' "$(cat "$src/VERSION")" > "$out/bin/tsugumi"
                chmod +x "$out/bin/tsugumi"
                '''))
        tools = {t: shutil.which(t) for t in ("curl", "shasum", "sha256sum", "tar", "gzip", "mktemp", "cut", "rm", "mkdir", "mv", "ln", "cat", "chmod", "dirname", "readlink", "printf")}
        bin_dir = tmp_path / "tools"
        bin_dir.mkdir(exist_ok=True)
        for name, found in tools.items():
            if found and not (bin_dir / name).exists():
                (bin_dir / name).symlink_to(found)
        path = f"{stubs}:{bin_dir}" + (f":{path_extra}" if path_extra else "")
        env = {"HOME": str(home), "PATH": path, "TSUGUMI_INSTALL_BASE": release.as_uri()}
        return subprocess.run(command or ["/bin/sh", str(SCRIPT)], capture_output=True, text=True, env=env)

    install.home = home
    install.release = release
    return install


def test_a_mac_gets_the_command_in_local_bin_and_nothing_else(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    r = machine("Darwin", "arm64")
    assert r.returncode == 0, r.stderr
    command = machine.home / ".local" / "bin" / "tsugumi"
    assert subprocess.run([command], capture_output=True, text=True).stdout.strip() == "1.2.3"
    # Says where it is and that PATH lacks it, and leaves the shell's configuration alone.
    assert "PATH に入っていません" in r.stdout
    assert [p.name for p in machine.home.iterdir()] == [".local"]
    assert sorted(p.name for p in (machine.home / ".local" / "share" / "tsugumi").iterdir()) == ["1.2.3", "current"]


def test_a_folder_on_path_is_not_reported(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    r = machine("Darwin", "arm64", path_extra=str(machine.home / ".local" / "bin"))
    assert r.returncode == 0, r.stderr
    assert "PATH に入っていません" not in r.stdout


def test_another_checksum_installs_nothing(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3", sha="0" * 64)
    r = machine("Darwin", "arm64")
    assert r.returncode != 0
    assert "チェックサム" in r.stderr
    assert not (machine.home / ".local" / "bin" / "tsugumi").exists()
    assert list((machine.home / ".local" / "share" / "tsugumi").iterdir()) == []


def test_a_new_version_replaces_the_old_one(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.4")
    assert machine("Darwin", "arm64").returncode == 0
    assert sorted(p.name for p in (machine.home / ".local" / "share" / "tsugumi").iterdir()) == ["1.2.4", "current"]
    assert subprocess.run([machine.home / ".local" / "bin" / "tsugumi"], capture_output=True, text=True).stdout.strip() == "1.2.4"


@needs_server
def test_update_puts_the_latest_release_in_place(machine):
    # `tsugumi update` (after the wrapper's own flags) runs the release's install.sh.
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.0.0")
    assert machine("Darwin", "arm64").returncode == 0
    shutil.copy(SCRIPT, machine.release / "install.sh")
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.0.1")
    r = machine("Darwin", "arm64", command=[shutil.which("node"), str(SERVER), "--open", "--reuse", "update"])
    assert r.returncode == 0, r.stderr
    assert subprocess.run([machine.home / ".local" / "bin" / "tsugumi"], capture_output=True, text=True).stdout.strip() == "1.0.1"
    assert "起動し直す" in r.stdout


@needs_server
def test_update_without_the_release_script_says_so_and_keeps_what_is_there(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.0.0")
    assert machine("Darwin", "arm64").returncode == 0
    r = machine("Darwin", "arm64", command=[shutil.which("node"), str(SERVER), "--open", "--reuse", "update"])
    assert r.returncode != 0
    assert "install.sh を取得できませんでした" in r.stderr
    assert subprocess.run([machine.home / ".local" / "bin" / "tsugumi"], capture_output=True, text=True).stdout.strip() == "1.0.0"


@pytest.mark.parametrize("arch", ["x86_64", "aarch64"])
def test_linux_builds_the_source_with_nix(machine, arch):
    artifact(machine.release, "tsugumi-linux.tar.gz", "1.2.3")
    r = machine("Linux", arch, nix=True)
    assert r.returncode == 0, r.stderr
    assert subprocess.run([machine.home / ".local" / "bin" / "tsugumi"], capture_output=True, text=True).stdout.strip() == "built 1.2.3"


def test_what_is_not_supported_says_so(machine):
    assert "Nix が必要" in machine("Linux", "x86_64").stderr
    assert "macOS 26" in machine("Darwin", "arm64", macos="15.5").stderr
    assert "Apple silicon" in machine("Darwin", "x86_64").stderr
    assert "対応していません" in machine("FreeBSD", "amd64").stderr
    assert not (machine.home / ".local").exists()


def test_the_release_publishes_what_the_script_takes():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    script = SCRIPT.read_text()
    assert "cli/install.sh cli/install.ps1" in workflow
    for name in re.findall(r"artifact=(\S+)", script):
        assert f"dist/{name} dist/{name}.sha256" in workflow, name


def test_windows_installs_in_wsl_and_leaves_a_command_that_starts_it_there():
    script = (ROOT / "cli" / "install.ps1").read_text()
    # `irm | iex` does not read it as UTF-8.
    assert script.isascii()
    # Runs install.sh in the distribution, with no shell of WSL's own expanding anything first.
    assert """wsl.exe --exec env "TSUGUMI_INSTALL_BASE=$base" bash -lc 'curl -fsSL $TSUGUMI_INSTALL_BASE/install.sh | sh'""" in script
    assert "wsl.exe --cd ~ --exec .local/bin/tsugumi %*" in script
    assert "wsl.exe -- " not in script
    # Says what is missing, and does not change PATH.
    assert "wsl --install" in script and "Nix is needed" in script
    assert "SetEnvironmentVariable" not in script


# ---- The web server on the VM -------------------------------------------------

HOST = r'''
import fcntl, hashlib, json, os, pathlib, socket, sys, threading
support = pathlib.Path(sys.argv[sys.argv.index("--support") + 1]).resolve()
lock = open(support / "vmhost.lock", "a")
try:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.exit(0)
endpoint = pathlib.Path("/tmp") / ("tsugumi-vmhost-%d-%s.sock" % (os.getuid(), hashlib.sha256(str(support).encode()).hexdigest()[:24]))
if endpoint.exists():
    endpoint.unlink()
listener = socket.socket(socket.AF_UNIX)
listener.bind(str(endpoint))
endpoint.chmod(0o600)
listener.listen()
pathlib.Path(os.environ["HOST_PID"]).write_text(str(os.getpid()))
prefs_file, log_file = os.environ["HOST_PREFS"], os.environ["HOST_LOG"]
mutex = threading.Lock()
def serve(peer):
    with peer, peer.makefile("r") as lines:
        for line in lines:
            with mutex:
                r = json.loads(line)
                with open(log_file, "a") as log:
                    log.write(line)
                with open(prefs_file) as source:
                    prefs = json.load(source)
                a = {"id": r["id"], "result": {}}
                if r["op"] == "prefs":
                    a["result"] = {k: prefs[k] for k in r["keys"] if k in prefs}
                elif r["op"] == "setPrefs":
                    prefs.update(r["values"])
                    temporary = prefs_file + ".new"
                    with open(temporary, "w") as target:
                        json.dump(prefs, target)
                    os.replace(temporary, prefs_file)
                elif r["op"] == "start":
                    if os.path.exists(os.environ["HOST_BUSY"]):
                        a = {"id": r["id"], "error": "fixture VM failed", "recovery": "Retry the virtual machine"}
                    else:
                        a["result"] = {"endpoint": "ws://127.0.0.1:1/rpc", "token": "t", "projectId": "p", "serverInstanceId": "s", "readyFile": "/r"}
                peer.sendall((json.dumps(a) + "\n").encode())
while True:
    peer, _ = listener.accept()
    threading.Thread(target=serve, args=(peer,), daemon=True).start()
'''


@pytest.fixture
def vm_server(tmp_path):
    support = tmp_path / "support"
    support.mkdir()
    prefs = tmp_path / "prefs.json"
    prefs.write_text(json.dumps({"agent.provider": "openai", "agent.model.openai": "from-the-app", "agent.permissions.allowed": ["network", "nix_source"]}))
    # 2001-01-01 + 800000000 s, as the app's JSONEncoder writes it.
    project = tmp_path / "project"
    project.mkdir()
    (project / "tsugumi.json").write_text('{"name": "P"}')
    (support / "recent-projects.json").write_text(json.dumps([{"path": str(project), "name": "P", "openedAt": 800000000.5}]))
    executable(tmp_path / "host", f"#!{sys.executable}\n{HOST}")
    env = isolated_environment(tmp_path, TSUGUMI_VMHOST=str(tmp_path / "host"), TSUGUMI_VM_IMAGE="/image", TSUGUMI_VM_SOURCE="/source", TSUGUMI_SUPPORT_DIR=str(support),
               TSUGUMI_WEB_DATA=str(support / "web"), HOST_PREFS=str(prefs), HOST_LOG=str(tmp_path / "log"), HOST_BUSY=str(tmp_path / "busy"), HOST_PID=str(tmp_path / "host.pid"))
    p = subprocess.Popen(["node", str(SERVER), "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        m = re.search(r"http://127\.0\.0\.1:(\d+)/\?key=([0-9a-f]+)", p.stdout.readline())
        assert m
        yield tmp_path, project, prefs, int(m.group(1)), f"tsugumi_{m.group(1)}={m.group(2)}"
    finally:
        p.terminate()
        p.wait(10)
        stop_resident_services(tmp_path)
        if (tmp_path / "host.pid").exists():
            try:
                os.kill(int((tmp_path / "host.pid").read_text()), signal.SIGTERM)
            except ProcessLookupError:
                pass
        endpoint = Path("/tmp") / f"tsugumi-vmhost-{os.getuid()}-{hashlib.sha256(str(support.resolve()).encode()).hexdigest()[:24]}.sock"
        endpoint.unlink(missing_ok=True)


def api(port: int, cookie: str, method: str, route: str, body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, route, body=json.dumps(body or {}) if method == "POST" else None,
              headers={"Host": f"127.0.0.1:{port}", "Cookie": cookie, "X-Tsugumi": "1", "Content-Type": "application/json"})
    r = c.getresponse()
    data = json.loads(r.read())
    c.close()
    assert r.status == 200, data
    return data


@needs_server
def test_the_page_needs_no_setup_and_may_share_folders(vm_server):
    _, _, _, port, cookie = vm_server
    s = api(port, cookie, "GET", "/api/status")
    assert all(s["setup"][k]["ok"] for k in ("tsugumid", "worker", "sdk"))
    assert s["hostFolders"] is True


@needs_server
def test_recent_projects_are_the_apps_in_its_format(vm_server):
    tmp, project, _, port, cookie = vm_server
    s = api(port, cookie, "GET", "/api/status")
    assert [r["path"] for r in s["recents"]] == [str(project)]
    assert s["recents"][0]["openedAt"] == (800000000.5 + 978307200) * 1000
    before = time.time()
    api(port, cookie, "POST", "/api/project/open", {"path": str(project)})
    saved = json.loads((tmp / "support" / "recent-projects.json").read_text())
    assert saved[0]["path"] == str(project.resolve())
    assert abs(saved[0]["openedAt"] + 978307200 - before) < 30
    assert not (tmp / "support" / "web" / "recent-projects.json").exists()


@needs_server
def test_settings_are_the_apps_and_only_what_changed_is_written(vm_server):
    _, _, prefs, port, cookie = vm_server
    s = api(port, cookie, "GET", "/api/settings")
    assert (s["provider"], s["openAIModel"]) == ("openai", "from-the-app")
    api(port, cookie, "POST", "/api/settings", {"model": "chosen-here", "webSearch": True})
    api(port, cookie, "GET", "/api/settings")
    p = json.loads(prefs.read_text())
    assert p["agent.model"] == "chosen-here" and p["agent.webSearch"] is True
    # Left alone: at the app's own default, not this server's.
    assert "agent.model.chatgpt" not in p and "agent.effort" not in p
    # A kind of permission only the app knows stays when the page changes the list.
    api(port, cookie, "POST", "/api/settings", {"allowedPermissions": ["network", "sandbox_off"]})
    api(port, cookie, "GET", "/api/settings")
    assert sorted(json.loads(prefs.read_text())["agent.permissions.allowed"]) == ["network", "nix_source", "sandbox_off"]


@needs_server
def test_a_project_starts_through_the_helper_with_the_pages_origins(vm_server):
    tmp, project, _, port, cookie = vm_server
    s = api(port, cookie, "POST", "/api/env/start", {"path": str(project)})
    assert s["state"] == "ready" and s["endpoint"] == "ws://127.0.0.1:1/rpc" and s["token"] == "t"
    start = [json.loads(l) for l in (tmp / "log").read_text().splitlines() if '"start"' in l][0]
    assert start["origins"] == [f"http://127.0.0.1:{port}", f"http://localhost:{port}"]


@needs_server
def test_a_vm_start_failure_preserves_the_hosts_recovery_message(vm_server):
    tmp, project, _, port, cookie = vm_server
    (tmp / "busy").touch()
    s = api(port, cookie, "POST", "/api/env/start", {"path": str(project)})
    assert s["state"] == "failed" and s["message"] == "fixture VM failed" and s["recovery"] == "Retry the virtual machine"


# ---- Running the command again ---------------------------------------------------

@needs_server
def test_a_copy_of_a_read_only_demo_can_be_edited(tmp_path):
    # On Linux the bundled demos are in the Nix store.
    root = tmp_path / "share"
    shutil.copytree(SERVER.parent, root / "web" / "dist", ignore=shutil.ignore_patterns("public", "*.map"))
    demo = root / "demos" / "ja" / "d" / "definitions" / "c"
    demo.mkdir(parents=True)
    (demo / "_card.json").write_text("{}")
    (root / "demos" / "ja" / "d" / "tsugumi.json").write_text('{"name": "D"}')
    (root / "demos" / "ja" / "index.json").write_text('{"demos": [{"id": "d", "title": "D", "path": "d"}]}')
    for f in sorted(root.joinpath("demos").rglob("*"), reverse=True):
        f.chmod(0o555 if f.is_dir() else 0o444)
    env = isolated_environment(tmp_path, TSUGUMI_WEB_DATA=str(tmp_path / "data"), TSUGUMI_PROJECTS_DIR=str(tmp_path / "projects"))
    p = subprocess.Popen(["node", str(root / "web" / "dist" / "server.js"), "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        m = re.search(r":(\d+)/\?key=([0-9a-f]+)", p.stdout.readline())
        copy = Path(api(int(m.group(1)), f"tsugumi_{m.group(1)}={m.group(2)}", "POST", "/api/demo/copy", {"id": "d"})["path"])
        (copy / "new").mkdir()
        (copy / "definitions" / "c" / "new").mkdir()
        (copy / "tsugumi.json").write_text("{}")
    finally:
        p.terminate()
        p.wait(10)
        stop_resident_services(tmp_path)
        for f in root.joinpath("demos").rglob("*"):
            if f.is_dir():
                f.chmod(0o755)


@needs_server
def test_a_second_run_opens_a_page_on_the_running_server(tmp_path):
    data = tmp_path / "data"
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    opened = tmp_path / "opened"
    for name in ("open", "xdg-open"):
        executable(stubs / name, f'#!/bin/sh\necho "$1" >> {opened}\n')
    env = isolated_environment(tmp_path, TSUGUMI_WEB_DATA=str(data), PATH=f"{stubs}:{os.environ['PATH']}")
    first = subprocess.Popen(["node", str(SERVER), "--port", "0", "--reuse"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        port = int(re.search(r":(\d+)/\?key=", first.stdout.readline()).group(1))
        deadline = time.monotonic() + 10
        while not (data / "server.json").exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        r = subprocess.run(["node", str(SERVER), "--open", "--reuse"], capture_output=True, text=True, env=env, timeout=30)
        assert r.returncode == 0, r.stderr
        assert "動いています" in r.stdout
        while not opened.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert re.fullmatch(rf"http://127\.0\.0\.1:{port}/\?key=[0-9a-f]+\n", opened.read_text())
    finally:
        first.terminate()
        first.wait(10)
        stop_resident_services(tmp_path)


@needs_server
@pytest.mark.skipif(sys.platform == "darwin", reason="the Mac takes `open`, whatever the environment says")
def test_in_wsl_the_page_opens_in_the_browser_of_windows(tmp_path):
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    opened = tmp_path / "opened"
    executable(stubs / "rundll32.exe", f'#!/bin/sh\necho "$@" >> {opened}\n')
    env = isolated_environment(tmp_path, TSUGUMI_WEB_DATA=str(tmp_path / "data"), WSL_DISTRO_NAME="Ubuntu", PATH=f"{stubs}:{os.environ['PATH']}")
    p = subprocess.Popen(["node", str(SERVER), "--port", "0", "--open"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        port = int(re.search(r":(\d+)/\?key=", p.stdout.readline()).group(1))
        deadline = time.monotonic() + 10
        while not opened.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert re.fullmatch(rf"url\.dll,FileProtocolHandler http://127\.0\.0\.1:{port}/\?key=[0-9a-f]+\n", opened.read_text())
    finally:
        p.terminate()
        p.wait(10)
        stop_resident_services(tmp_path)


@needs_server
def test_a_folder_after_the_commands_flags_is_the_project_to_open(tmp_path):
    project = tmp_path / "project"
    project.mkdir()
    (project / "tsugumi.json").write_text("{}")
    env = isolated_environment(tmp_path, TSUGUMI_WEB_DATA=str(tmp_path / "data"))
    # As cli/tsugumi-macos and nix/cli.nix give it, without --open (no browser here).
    p = subprocess.Popen(["node", str(SERVER), "--reuse", str(project), "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        assert "&project=" in p.stdout.readline()
    finally:
        p.terminate()
        p.wait(10)
        stop_resident_services(tmp_path)


@needs_server
def test_the_usual_port_taken_means_another_port(tmp_path):
    with socket.socket() as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", 8780))
            s.listen()
        except OSError:
            pass  # taken already, as wanted
        env = isolated_environment(tmp_path, TSUGUMI_WEB_DATA=str(tmp_path / "data"))
        env.pop("TSUGUMI_WEB_PORT", None)
        p = subprocess.Popen(["node", str(SERVER)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        try:
            port = int(re.search(r":(\d+)/\?key=", p.stdout.readline()).group(1))
            assert port != 8780
        finally:
            p.terminate()
            p.wait(10)
        stop_resident_services(tmp_path)
