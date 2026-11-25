"""The `tsugumi` command: what cli/install.sh does on each platform (uname and
Nix are stand-ins here, so all of it runs anywhere), `tsugumi web` with the
Mac's VM backend against a stand-in for tsugumi-vmhost, and runs of the command
that share one host. A real VM is checked by hand (tests/README.md)."""

from __future__ import annotations

import hashlib
import http.client
import json
import os
import re
import shutil
import shlex
import socket
import stat
import struct
import subprocess
import sys
import tarfile
import textwrap
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest

from conftest import CLI, HOSTD, WEB_ROOT, BUILD, WORKER, NIX, write_project, write_card
from test_claude_code_plugin import isolated_environment, stop_resident_services
from harness.file_events import process_file, wait_for_file

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "cli" / "install.sh"
needs_cli = pytest.mark.skipif(not CLI.exists(), reason="needs build/cli/tsugumi")
needs_server = pytest.mark.skipif(not CLI.exists() or not HOSTD.exists() or not (WEB_ROOT / "index.html").exists(),
                                  reason="needs build/cli/tsugumi, tsugumi-hostd and ui/web/dist (cd ui/web && npm run build)")


def executable(path: Path, text: str):
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


def artifact(release: Path, name: str, version: str, sha: str | None = None):
    """A release file: tsugumi/ with VERSION and a command that prints it."""
    tree = release / "build" / version / "tsugumi"
    tree.mkdir(parents=True)
    (tree / "VERSION").write_text(version + '''
''')
    executable(tree / "tsugumi", '''#!/bin/sh
cat "$(dirname "$(readlink -f "$0")")/VERSION"
''')
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
                printf '#!/bin/sh\\necho %s\\n' "$(cat "$src/VERSION")" > "$out/bin/tsugumi"
                chmod +x "$out/bin/tsugumi"
                '''))
        tools = {t: shutil.which(t) for t in ("curl", "shasum", "sha256sum", "tar", "gzip", "mktemp", "cut", "rm", "mkdir", "mv", "ln", "cat", "chmod", "dirname", "readlink", "printf")}
        bin_dir = tmp_path / "tools"
        bin_dir.mkdir(exist_ok=True)
        for name, found in tools.items():
            if found and not (bin_dir / name).exists():
                (bin_dir / name).symlink_to(found)
        # Keep the real tools: the simulated OS need not match the userland on
        # PATH (notably GNU coreutils from Nix on macOS).
        path = f"{stubs}:{bin_dir}" + (f":{path_extra}" if path_extra else "")
        env = {"HOME": str(home), "PATH": path, "TSUGUMI_INSTALL_BASE": release.as_uri()}
        return subprocess.run(command or ["/bin/sh", str(SCRIPT)], capture_output=True, text=True, env=env)

    install.home = home
    install.release = release
    install.stubs = stubs
    return install


def test_a_mac_gets_the_command_in_local_bin_and_nothing_else(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    r = machine("Darwin", "arm64")
    assert r.returncode == 0, r.stderr
    command = machine.home / ".local" / "bin" / "tsugumi"
    assert subprocess.run([command], capture_output=True, text=True).stdout.strip() == "1.2.3"
    # Says where it is and that PATH lacks it, and leaves the shell's configuration alone.
    assert re.search(rf"(?<!\S){re.escape(str(command.parent))}(?=\s|$)", r.stdout)
    assert [p.name for p in machine.home.iterdir()] == [".local"]
    data = machine.home / ".local/share/tsugumi"
    current = (data / "current").resolve()
    assert current.is_relative_to(data)
    assert subprocess.check_output([current / "tsugumi"], text=True).strip() == "1.2.3"


def test_a_folder_on_path_is_not_reported(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    r = machine("Darwin", "arm64", path_extra=str(machine.home / ".local" / "bin"))
    assert r.returncode == 0, r.stderr
    directory = re.escape(str(machine.home / ".local/bin"))
    assert not re.search(rf"(?<!\S){directory}(?=\s|$)", r.stdout)


def test_another_checksum_installs_nothing(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3", sha="0" * 64)
    r = machine("Darwin", "arm64")
    assert r.returncode == 65
    assert not (machine.home / ".local" / "bin" / "tsugumi").exists()
    assert list((machine.home / ".local" / "share" / "tsugumi").iterdir()) == []


def test_a_new_version_replaces_the_old_one(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    data = machine.home / ".local/share/tsugumi"
    previous = (data / "current").resolve()
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.4")
    assert machine("Darwin", "arm64").returncode == 0
    current = (data / "current").resolve()
    assert current != previous
    assert subprocess.check_output([current / "tsugumi"], text=True).strip() == "1.2.4"
    assert subprocess.check_output([previous / "tsugumi"], text=True).strip() == "1.2.3"
    assert subprocess.run([machine.home / ".local" / "bin" / "tsugumi"], capture_output=True, text=True).stdout.strip() == "1.2.4"


def test_failed_same_version_build_keeps_the_current_command(machine):
    artifact(machine.release, "tsugumi-linux.tar.gz", "1.2.3")
    assert machine("Linux", "x86_64", nix=True).returncode == 0
    command = machine.home / ".local/bin/tsugumi"
    before = subprocess.check_output([command], text=True)
    executable(machine.stubs / "nix", "#!/bin/sh\nexit 1\n")
    result = machine("Linux", "x86_64")
    assert result.returncode != 0
    assert subprocess.check_output([command], text=True) == before


def test_update_preserves_foreign_directories_and_old_runtime_files(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    data = machine.home / ".local/share/tsugumi"
    old = (data / "current").resolve()
    foreign = data / "research"
    foreign.mkdir()
    (foreign / "notes").write_text("keep")
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.4")
    assert machine("Darwin", "arm64").returncode == 0
    assert (foreign / "notes").read_text() == "keep"
    assert (old / "tsugumi").exists()


def test_reinstalling_the_same_version_keeps_the_previous_generation(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    data = machine.home / ".local/share/tsugumi"
    previous = (data / "current").resolve()
    assert machine("Darwin", "arm64").returncode == 0
    assert (data / "current").resolve() != previous
    assert subprocess.check_output([previous / "tsugumi"], text=True).strip() == "1.2.3"
    assert subprocess.check_output([machine.home / ".local/bin/tsugumi"], text=True).strip() == "1.2.3"


def test_failed_version_check_keeps_the_current_command(machine):
    name = "tsugumi-linux.tar.gz"
    artifact(machine.release, name, "1.2.3")
    assert machine("Linux", "aarch64", nix=True).returncode == 0
    # The build succeeded, but the resulting command reports a different version.
    artifact(machine.release, name, "1.2.4")
    nix = machine.stubs / "nix"
    executable(nix, nix.read_text().replace('$(cat "$src/VERSION")', "wrong"))
    result = machine("Linux", "aarch64")
    assert result.returncode == 70
    assert subprocess.check_output([machine.home / ".local/bin/tsugumi"], text=True).strip() == "1.2.3"
    assert len(list((machine.home / ".local/share/tsugumi").iterdir())) == 2


@pytest.mark.parametrize("target", [".local/bin/tsugumi", ".local/share/tsugumi/current"])
def test_failed_link_switch_keeps_the_current_command(machine, target):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    data = machine.home / ".local/share/tsugumi"
    previous = (data / "current").readlink()
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.4")
    # Fail only the final rename, after the real utility's capability probe.
    executable(machine.stubs / "mv", f'''#!/bin/sh
for destination do :; done
[ "$destination" != "$HOME/{target}" ] || exit 1
exec "{shutil.which("mv")}" "$@"
''')
    result = machine("Darwin", "arm64")
    assert result.returncode != 0
    assert (data / "current").readlink() == previous
    assert subprocess.check_output([machine.home / ".local/bin/tsugumi"], text=True).strip() == "1.2.3"
    assert sorted(p.name for p in data.iterdir()) == sorted([str(previous), "current"])
    assert [p.name for p in (machine.home / ".local/bin").iterdir()] == ["tsugumi"]


@needs_cli
def test_update_puts_the_latest_release_in_place(machine):
    # `tsugumi update` (after the wrapper's own flags) runs the release's install.sh.
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.0.0")
    assert machine("Darwin", "arm64").returncode == 0
    previous = (machine.home / ".local/share/tsugumi/current").resolve()
    shutil.copy(SCRIPT, machine.release / "install.sh")
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.0.1")
    r = machine("Darwin", "arm64", command=[str(CLI), "update"])
    assert r.returncode == 0, r.stderr
    assert subprocess.check_output([previous / "tsugumi"], text=True).strip() == "1.0.0"
    assert subprocess.run([machine.home / ".local" / "bin" / "tsugumi"], capture_output=True, text=True).stdout.strip() == "1.0.1"


@needs_cli
def test_update_without_the_release_script_says_so_and_keeps_what_is_there(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.0.0")
    assert machine("Darwin", "arm64").returncode == 0
    r = machine("Darwin", "arm64", command=[str(CLI), "update"])
    assert r.returncode == 69
    assert subprocess.run([machine.home / ".local" / "bin" / "tsugumi"], capture_output=True, text=True).stdout.strip() == "1.0.0"


@pytest.mark.parametrize("arch", ["x86_64", "aarch64"])
def test_linux_builds_the_source_with_nix(machine, arch):
    artifact(machine.release, "tsugumi-linux.tar.gz", "1.2.3")
    r = machine("Linux", arch, nix=True)
    assert r.returncode == 0, r.stderr
    assert subprocess.run([machine.home / ".local" / "bin" / "tsugumi"], capture_output=True, text=True).stdout.strip() == "1.2.3"


def test_what_is_not_supported_says_so(machine):
    assert machine("Linux", "x86_64").returncode == 69
    assert machine("Darwin", "arm64", macos="15.5").returncode == 69
    assert machine("Darwin", "x86_64").returncode == 69
    assert machine("FreeBSD", "amd64").returncode == 69
    assert not (machine.home / ".local").exists()


def test_the_release_publishes_what_the_script_takes():
    workflow = (ROOT / ".github/workflows/release.yml").read_text()
    script = SCRIPT.read_text()
    start = workflow.index("gh release create ")
    command = workflow[start:].splitlines()
    release = []
    for line in command:
        release.extend(shlex.split(line.removesuffix("\\").strip()))
        if not line.endswith("\\"):
            break
    assets = {Path(item).name for item in release if item.startswith(("cli/", "dist/"))}
    assert {"install.sh", "install.ps1"} <= assets
    for name in re.findall(r"^\s*artifact=(\S+)", script, re.MULTILINE):
        assert {name, name + ".sha256"} <= assets


def test_windows_installer_is_ascii_for_irm_decoding():
    assert (ROOT / 'cli/install.ps1').read_text().isascii()


@pytest.mark.skipif(os.name != 'nt', reason='Windows launcher behavior requires Windows and WSL command doubles')
def test_windows_launcher_forwards_arguments_and_preserves_failed_uninstall(tmp_path):
    # The real installer is run with a recording WSL function; it writes the launcher.
    import json
    powershell = shutil.which('pwsh') or shutil.which('powershell')
    if not powershell:
        pytest.skip('PowerShell is required')
    profile = tmp_path / 'profile'
    profile.mkdir()
    log = tmp_path / 'wsl.jsonl'
    driver = tmp_path / 'install.ps1'
    installer = str(ROOT / 'cli/install.ps1').replace("'", "''")
    driver.write_text("function wsl.exe { $args | ConvertTo-Json -Compress | Add-Content $env:WSL_LOG; $global:LASTEXITCODE=0 }; . '" + installer + "'", encoding='ascii')
    environment = dict(os.environ, USERPROFILE=str(profile), WSL_LOG=str(log))
    result = subprocess.run([powershell, '-NoProfile', '-File', str(driver)], env=environment, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    requests = [json.loads(line) for line in log.read_text().splitlines()]
    assert all(request[0] == '--exec' for request in requests)
    launcher = profile / '.local/bin/tsugumi.cmd'
    assert launcher.is_file()
    # cmd resolves the recording WSL command first; preview/failure keep the launcher.
    double = tmp_path / 'wsl.cmd'
    double.write_text('@echo off\necho %*>>"%WSL_ARGS%"\nexit /b %WSL_STATUS%\n', encoding='ascii')
    environment.update(PATH=str(tmp_path) + os.pathsep + environment['PATH'], WSL_ARGS=str(tmp_path / 'argv'), WSL_STATUS='1')
    failed = subprocess.run(['cmd', '/c', str(launcher), 'uninstall', '--yes'], env=environment)
    assert failed.returncode == 1 and launcher.exists()
    environment['WSL_STATUS'] = '0'
    subprocess.run(['cmd', '/c', str(launcher), 'uninstall'], env=environment, check=True)
    assert launcher.exists()
    subprocess.run(['cmd', '/c', str(launcher), 'uninstall', '--yes'], env=environment, check=True)
    assert not launcher.exists()
    assert '--cd ~ --exec .local/bin/tsugumi uninstall --yes' in (tmp_path / 'argv').read_text()


# ---- Uninstalling --------------------------------------------------------------

@needs_cli
@pytest.mark.parametrize("system,arch,nix", [("Darwin", "arm64", False), ("Linux", "x86_64", True)])
def test_uninstall_previews_then_removes_only_the_installed_command(machine, system, arch, nix):
    artifact(machine.release, "tsugumi-linux.tar.gz" if nix else "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine(system, arch, nix=nix).returncode == 0
    data = machine.home / ".local/share/tsugumi"
    command = machine.home / ".local/bin/tsugumi"
    release = (data / "current").resolve()
    kept = [machine.home / "Documents/Tsugumi/project/tsugumi.json",
            machine.home / "Library/Application Support/Tsugumi/vm.img",
            machine.home / ".config/tsugumi/settings.json", data / "my-notes.txt"]
    for f in kept:
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text("keep me")
    store = machine.home / "nix-store-fixture"
    store.mkdir()
    (store / "package").write_text("keep store output")
    (release / "store-output").symlink_to(store, target_is_directory=True)
    args = [str(CLI), "uninstall"]
    preview = machine(system, arch, nix=nix, command=args)
    assert preview.returncode == 0, preview.stderr
    assert "uninstall --yes" in preview.stdout
    assert command.exists() and release.exists()
    removed = machine(system, arch, nix=nix, command=args + ["--yes"])
    assert removed.returncode == 0, removed.stderr
    assert not command.is_symlink() and not (data / "current").is_symlink()
    assert not release.exists()
    assert all(f.read_text() == "keep me" for f in kept)
    assert (store / "package").read_text() == "keep store output"
    again = machine(system, arch, nix=nix, command=args + ["--yes"])
    assert again.returncode == 0
    assert all(f.read_text() == "keep me" for f in kept)


@needs_cli
def test_uninstall_removes_marked_generations_and_preserves_foreign_directories(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    data = machine.home / ".local/share/tsugumi"
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.4")
    assert machine("Darwin", "arm64").returncode == 0
    foreign = data / "research"
    foreign.mkdir()
    (foreign / "notes").write_text("keep")
    # A copied marker for another directory is not proof of ownership.
    (foreign / ".installed-by-tsugumi").write_text("another-directory")
    result = machine("Darwin", "arm64", command=[str(CLI), "uninstall", "--yes"])
    assert result.returncode == 0, result.stderr
    assert list(data.iterdir()) == [foreign]
    assert (foreign / "notes").read_text() == "keep"


@needs_cli
def test_uninstall_removes_empty_installation_folder(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    r = machine("Darwin", "arm64", command=[str(CLI), "uninstall", "--yes"])
    assert r.returncode == 0, r.stderr
    assert not (machine.home / ".local/share/tsugumi").exists()


@needs_cli
@pytest.mark.parametrize("change", ["launcher", "data", "bin", "current", "version", "marker"])
def test_uninstall_refuses_replaced_or_redirected_installations(machine, change):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    data = machine.home / ".local/share/tsugumi"
    command = machine.home / ".local/bin/tsugumi"
    release = (data / "current").resolve()
    external = machine.home / "external"
    external.mkdir()
    (external / "keep").write_text("keep")
    if change == "launcher":
        command.unlink()
        command.write_text("other command")
    elif change in ("data", "bin", "version"):
        target = {"data": data, "bin": command.parent, "version": release}[change]
        target.rename(external / "original")
        target.symlink_to(external / "original", target_is_directory=True)
    elif change == "current":
        (data / "current").unlink()
        (data / "current").symlink_to(external, target_is_directory=True)
    else:
        (release / ".installed-by-tsugumi").unlink()
    r = machine("Darwin", "arm64", command=[str(CLI), "uninstall", "--yes"])
    assert r.returncode != 0
    assert (external / "keep").read_text() == "keep"
    assert command.exists() or command.is_symlink()


@needs_cli
def test_uninstall_rejects_unknown_options_without_deleting(machine):
    artifact(machine.release, "tsugumi-macos-arm64.tar.gz", "1.2.3")
    assert machine("Darwin", "arm64").returncode == 0
    r = machine("Darwin", "arm64", command=[str(CLI), "uninstall", "--purge"])
    assert r.returncode == 64
    assert (machine.home / ".local/bin/tsugumi").exists()


# ---- `tsugumi web` with the VM ---------------------------------------------------


def web(env, *arguments, check=True):
    r = subprocess.run([str(CLI), "web", "--no-browser", *map(str, arguments)], capture_output=True, text=True, env=env, timeout=300)
    if check:
        assert r.returncode == 0, r.stderr
    return r


def page_url(output: str):
    url = urlparse(re.search(r"Tsugumi web UI: (\S+)", output).group(1))
    return url, url.port, parse_qs(url.query)


def hosts(state: Path) -> int:
    listing = subprocess.check_output(["ps", "-axww", "-o", "command="], text=True)
    return listing.count(f"--state-dir {state} ")


@pytest.fixture
def vm_server(tmp_path):
    from test_web_server import page_cookie
    support = tmp_path / "support"
    support.mkdir()
    project = tmp_path / "project"
    write_project(project)
    helper = (Path(__file__).parent / "vm_helper_double.py").read_text()
    executable(tmp_path / "host", f"#!{sys.executable}\n{helper}")
    env = isolated_environment(tmp_path, TSUGUMI_VMHOST=str(tmp_path / "host"), TSUGUMI_VM_IMAGE="/image", TSUGUMI_SUPPORT_DIR=str(support),
               TSUGUMI_PY_BINDINGS=os.environ.get("TSUGUMI_PY_BINDINGS", str(BUILD / "generated/python")), TSUGUMI_WORKER=str(WORKER), TSUGUMI_NIX_DIR=str(NIX), HOST_LOG=str(tmp_path / "log"), HOST_BUSY=str(tmp_path / "busy"))
    try:
        _, port, query = page_url(web(env).stdout)
        yield tmp_path, project, port, page_cookie(port, query["key"][0])
    finally:
        stop_resident_services(tmp_path, vm_helper=tmp_path / "host")


@needs_server
def test_the_page_needs_no_setup_and_may_share_folders(vm_server):
    from contextlib import closing
    from test_web_server import page_host
    _, _, port, cookie = vm_server
    with closing(page_host(port, cookie)) as host:
        home = host.call("HostHomeRequest")
    assert home.vm and home.hostFolders
    assert home.tsugumid.ok and home.worker.ok and home.nixDir.ok


@needs_server
def test_attach_restarts_the_guest_registers_and_reconnects_repeatedly(vm_server):
    # Production hostd, VM process adapter and tsugumid; only the hypervisor is
    # replaced by the helper.
    from contextlib import closing
    from harness.remote_worker import string
    from test_web_server import page_host
    from Tsugumi.Host.EnvironmentAction import EnvironmentAction
    from Tsugumi.Host.EnvironmentPhase import EnvironmentPhase
    from tsugumi_client import Client, new_id

    root, project, port, cookie = vm_server

    def host():
        connection = page_host(port, cookie)
        # The VM adapter owns startup and rollback; a test watchdog only
        # prevents a hung fixture from waiting indefinitely.
        connection.socket.settimeout(240)
        return closing(connection)

    def start():
        with host() as h:
            state = h.call("EnvironmentRequest", project=str(project), action=EnvironmentAction.Start,
                           origins=[f"http://127.0.0.1:{port}"])
        assert state.phase == EnvironmentPhase.Ready, state.message
        return string(state.endpoint), string(state.token), string(state.projectId)

    endpoint, token, project_id = start()
    assert endpoint.startswith("ws://127.0.0.1:") and token and project_id
    launches = (json.loads(line) for line in (root / "log").read_text().splitlines())
    launch = next(event for event in launches if event["op"] == "run")
    assert f"http://127.0.0.1:{port}" in set(launch["origins"])
    with Client(ws=endpoint) as client:
        previous = client.hello(token=token, project=project_id).serverInstanceId
        assert client.open(new_id()) is not None
    for index in range(3):
        folder = root / f"attached-{index}"
        folder.mkdir()
        (folder / "note.txt").write_text(f"folder {index}")
        with host() as h:
            result = h.call("AttachFoldersRequest", project=str(project.resolve()), folders=[str(folder), str(folder)])
        assert len(result.folders) == 1 and string(result.folders[0].folder) == str(folder)
        endpoint, token, project_id = start()
        # A fresh project session must authenticate and read its snapshot.
        with Client(ws=endpoint) as client:
            hello = client.hello(token=token, project=project_id)
            assert hello.serverInstanceId != previous
            previous = hello.serverInstanceId
            assert client.open(new_id()) is not None
        # Retrying an existing share returns the same registered identity.
        with host() as h:
            retry = h.call("AttachFoldersRequest", project=str(project.resolve()), folders=[str(folder)])
        assert string(retry.folders[0].resource) == string(result.folders[0].resource)
        endpoint, token, project_id = start()
        with Client(ws=endpoint) as client:
            assert client.hello(token=token, project=project_id).serverInstanceId == previous


@needs_server
def test_recent_projects_are_owned_by_the_host_core(vm_server):
    from contextlib import closing
    from harness.remote_worker import string
    from test_web_server import page_host
    tmp, project, port, cookie = vm_server
    other = tmp / "another"
    write_project(other)
    with closing(page_host(port, cookie)) as host:
        assert host.call("HostRecentListRequest").projects == []
        host.call("HostRecentRememberRequest", path=str(project))
        saved = json.loads((tmp / "host-core" / "recent-projects.json").read_text())
        assert saved[0]["path"] == str(project.resolve())
        host.call("HostRecentRememberRequest", path=str(other))
        host.call("HostRecentRememberRequest", path=str(project))
        recents = [string(p.path) for p in host.call("HostRecentListRequest").projects]
        assert recents == [str(project.resolve()), str(other.resolve())]
        host.call("HostRecentForgetRequest", paths=[str(project.resolve()), str(other.resolve())])
        assert host.call("HostRecentListRequest").projects == []


@needs_server
def test_a_vm_start_failure_preserves_the_hosts_recovery_message(vm_server):
    from contextlib import closing
    from harness.remote_worker import string
    from test_web_server import page_host
    from Tsugumi.Host.EnvironmentAction import EnvironmentAction
    from Tsugumi.Host.EnvironmentPhase import EnvironmentPhase
    tmp, project, port, cookie = vm_server
    (tmp / "busy").touch()
    with closing(page_host(port, cookie)) as host:
        state = host.call("EnvironmentRequest", project=str(project), action=EnvironmentAction.Start)
    assert state.phase == EnvironmentPhase.Failed
    assert string(state.message) == "fixture VM failed"
    assert string(state.recovery) == "Retry the virtual machine"


# ---- Running the command again ---------------------------------------------------

@needs_cli
def test_bare_tsugumi_prints_its_usage(tmp_path):
    r = subprocess.run([str(CLI)], capture_output=True, text=True, env=isolated_environment(tmp_path))
    assert r.returncode == 0 and "tsugumi web" in r.stdout and "tsugumi mcp" in r.stdout
    r = subprocess.run([str(CLI), "no-such-command"], capture_output=True, text=True, env=isolated_environment(tmp_path))
    assert r.returncode == 64
    stop_resident_services(tmp_path)


@needs_cli
@pytest.mark.skipif(sys.platform != "darwin", reason="the Mac support folders")
def test_a_build_from_source_uses_the_dev_apps_support_folder(tmp_path):
    """Like the app built from source (AppChannel.swift), only a release shares the release app's folder."""
    env = isolated_environment(tmp_path, TSUGUMI_WEB_ROOT=str(tmp_path / "page"), TSUGUMI_HOSTD=str(tmp_path / "hostd"))
    for key in ("TSUGUMI_HOST_STATE", "TSUGUMI_SUPPORT_DIR", "TSUGUMI_WEB_DATA"):
        env.pop(key)
    (tmp_path / "page").mkdir()
    (tmp_path / "page" / "index.html").write_text("")
    # A stand-in host that records how it was started and never becomes ready.
    executable(tmp_path / "hostd", f'#!/bin/sh\nprintf "%s\\n" "$@" > "{tmp_path / "args"}"\n')
    r = subprocess.run([str(CLI), "web", "--no-browser"], capture_output=True, text=True, env=env)
    assert r.returncode != 0 and "did not start" in r.stderr
    args = (tmp_path / "args").read_text().splitlines()
    support = Path(env["HOME"]) / "Library/Application Support"
    assert args[args.index("--state-dir") + 1] == str(support / "Tsugumi-dev/host-core")
    assert not (support / "Tsugumi").exists()


@needs_server
def test_a_copy_of_a_read_only_demo_can_be_edited(tmp_path):
    from contextlib import closing
    from harness.remote_worker import string
    from test_web_server import page_cookie, page_host
    # On Linux the bundled demos are in the Nix store.
    root = tmp_path / "share"
    demo = root / "demos" / "ja" / "d" / "definitions" / "c"
    demo.mkdir(parents=True)
    (root / "VERSION").write_text("0.0.0\n")
    (demo / "_card.json").write_text('{"type": "code"}')
    (root / "demos" / "ja" / "d" / ".tsugumi").mkdir()
    (root / "demos" / "ja" / "d" / ".tsugumi" / "history").write_text("private")
    write_project(root / "demos" / "ja" / "d", {"name": "D"})
    (root / "demos" / "ja" / "index.json").write_text('{"demos": [{"id": "d", "title": "D", "path": "d"}]}')
    for f in sorted(root.joinpath("demos").rglob("*"), reverse=True):
        f.chmod(0o555 if f.is_dir() else 0o444)
    env = isolated_environment(tmp_path, TSUGUMI_REPO=str(root), TSUGUMI_DEMOS_DIR=str(root / "demos"),
                               TSUGUMI_PROJECTS_DIR=str(tmp_path / "projects"))
    try:
        _, port, query = page_url(web(env).stdout)
        (tmp_path / "projects").mkdir()
        with closing(page_host(port, page_cookie(port, query["key"][0]))) as host:
            copy = Path(string(host.call("HostDemoCopyRequest", id="d", parent=str(tmp_path / "projects"), language="ja").path))
        assert not (copy / ".tsugumi").exists()
        (copy / "new").mkdir()
        (copy / "definitions" / "c" / "new").mkdir()
        (copy / "tsugumi.json").write_text("{}")
    finally:
        stop_resident_services(tmp_path)
        for f in root.joinpath("demos").rglob("*"):
            if f.is_dir():
                f.chmod(0o755)


@needs_server
def test_a_second_run_shows_the_project_on_the_running_host(tmp_path):
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    opened = tmp_path / "opened"
    for name in ("open", "xdg-open"):
        executable(stubs / name, f'#!/bin/sh\nprintf "%s\\n" "$1" > {opened}.tmp\nmv {opened}.tmp {opened}\n')
    project = tmp_path / "project with spaces"
    write_project(project)
    env = isolated_environment(tmp_path, PATH=f"{stubs}:{os.environ['PATH']}")
    try:
        _, port, _ = page_url(web(env).stdout)
        state = tmp_path / "host-core"
        pid = json.loads((state / "host.ready").read_text())["pid"]
        r = subprocess.run([str(CLI), "web", str(project)], capture_output=True, text=True, env=env, timeout=300)
        assert r.returncode == 0, r.stderr
        assert json.loads((state / "host.ready").read_text())["pid"] == pid
        # No page is attached: hostd opens the Web UI with the system's opener.
        wait_for_file(opened)
        shown = urlparse(opened.read_text().strip())
        assert shown.port == port
        assert parse_qs(shown.query)["project"] == [str(project.resolve())]
        assert hosts(state) == 1
    finally:
        stop_resident_services(tmp_path)


@needs_server
def test_simultaneous_commands_end_with_one_host(tmp_path):
    env = isolated_environment(tmp_path)
    try:
        processes = [subprocess.Popen([str(CLI), "web", "--no-browser"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, env=env) for _ in range(2)]
        outputs = [p.communicate(timeout=300)[0] for p in processes]
        assert all(p.returncode == 0 for p in processes), outputs
        assert len({page_url(o)[1] for o in outputs}) == 1
        assert hosts(tmp_path / "host-core") == 1
    finally:
        stop_resident_services(tmp_path)


@needs_server
@pytest.mark.skipif(sys.platform == "darwin", reason="the Mac takes `open`, whatever the environment says")
def test_in_wsl_the_page_opens_in_the_browser_of_windows(tmp_path):
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    opened = tmp_path / "opened"
    executable(stubs / "rundll32.exe", f'#!/bin/sh\nprintf "%s\\n" "$@" > {opened}.tmp\nmv {opened}.tmp {opened}\n')
    env = isolated_environment(tmp_path, WSL_DISTRO_NAME="Ubuntu", PATH=f"{stubs}:{os.environ['PATH']}")
    try:
        r = subprocess.run([str(CLI), "web"], capture_output=True, text=True, env=env, timeout=300)
        assert r.returncode == 0, r.stderr
        wait_for_file(opened)
        handler, url = opened.read_text().splitlines()
        assert handler == "url.dll,FileProtocolHandler"
        shown = urlparse(url)
        assert shown.scheme == "http" and shown.hostname == "127.0.0.1"
        assert shown.port == page_url(r.stdout)[1]
        assert shown.path == "/" and parse_qs(shown.query)["key"][0]
    finally:
        stop_resident_services(tmp_path)


@needs_server
def test_a_folder_after_the_commands_flags_is_the_project_to_show(tmp_path):
    project = tmp_path / "project"
    write_project(project)
    env = isolated_environment(tmp_path)
    try:
        assert parse_qs(page_url(web(env, project).stdout)[0].query)["project"] == [str(project.resolve())]
    finally:
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
        env = isolated_environment(tmp_path)
        env.pop("TSUGUMI_WEB_PORT", None)
        try:
            assert page_url(web(env).stdout)[1] != 8780
        finally:
            stop_resident_services(tmp_path)

@needs_cli
def test_lock_update_pins_a_local_archive_through_the_agent_tool(tmp_path):
    from harness.daemon import Daemon
    from harness.definition_evaluation import EMPTY_OUTPUTS
    from test_claude_code_plugin import UserAgent
    from tsugumi_client import path, text

    project = tmp_path / "project"
    contents = tmp_path / "library"
    contents.mkdir()
    (contents / "default.nix").write_text('"from the fixture archive"')
    archive = tmp_path / "library.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(contents, arcname="library")
    url = "tarball+" + archive.resolve().as_uri()
    write_project(project, {
        "name": "locked fixture",
        "nixPath": [{"name": "lib", "url": url}],
        "canvas": {"cards": [{"id": "work"}], "connections": []},
    })
    definition = project / "definitions/work"
    definition.mkdir(parents=True)
    write_card(definition)
    (definition / "_outputs.fbs").write_text(EMPTY_OUTPUTS)
    (definition / "default.nix").write_text("{ params, inputs }: { }")
    env = isolated_environment(tmp_path, TSUGUMI_NIX_DIR=str(NIX.resolve()))

    def update(*args):
        return subprocess.run([str(CLI.resolve()), "lock", "update", *args, "--project", str(project)],
                              env=env, capture_output=True, text=True, timeout=30)

    def approved_update(*args):
        for _ in range(4):
            updated = update(*args)
            if updated.returncode == 0:
                return
            assert updated.stdout, updated.stderr
            request = json.loads(updated.stdout)
            assert request["code"] == "grant_required", updated.stdout + updated.stderr
            user = UserAgent(tmp_path)
            try:
                user.call("AccessResolveRequest", request_id=request["request_id"], allow=True)
            finally:
                user.close()
        pytest.fail(updated.stdout + updated.stderr)

    try:
        approved_update("lib")
        lock = project / "tsugumi.lock"
        entry, = json.loads(lock.read_text())["entries"]
        assert entry["name"] == "lib"
        assert entry["url"] == url
        assert entry["narHash"].startswith("sha256-")
        assert "rev" not in entry
        instant = lock.stat().st_mtime_ns
        approved_update()
        assert lock.stat().st_mtime_ns == instant
    finally:
        stop_resident_services(tmp_path)

    with Daemon(project) as daemon:
        result = daemon.connect().call("EvaluateNixRequest", card=path("work"),
                                       expression="import <lib>")
        assert result.failure is None, result.failure and text(result.failure.diagnostic)
        assert json.loads(text(result.json)) == "from the fixture archive"
