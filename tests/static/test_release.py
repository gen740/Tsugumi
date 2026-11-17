"""What a release build is made of: one version number (VERSION) for every
part, and no path of the build machine (#filePath) outside debug builds."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_every_part_takes_the_version_from_one_file():
    version = (ROOT / "VERSION").read_text().strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version)
    assert 'file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TSUGUMI_VERSION' in (ROOT / "CMakeLists.txt").read_text()
    assert "version = lib.fileContents ../VERSION;" in (ROOT / "nix/core.nix").read_text()
    assert "VERSION" in (ROOT / "macOS/Container/vm-source.txt").read_text().split()
    assert "VERSION" in (ROOT / "macOS/build-app.sh").read_text()


def test_the_app_finds_its_resources_on_any_mac():
    # the native build system's Bundle.module looks in the build folder, not in Contents/Resources
    script = (ROOT / "macOS/build-app.sh").read_text()
    assert "build_opts=(-c release --build-system swiftbuild --arch arm64)" in script
    assert 'grep -F "$repo/"' in script


def test_the_dev_build_can_be_signed_with_your_certificate_and_the_release_never_is():
    script = (ROOT / "macOS/build-app.sh").read_text()
    # ad-hoc unless a dev build is given an identity
    assert "\nidentity=-\n" in script
    assert 'if [ "$channel" = dev ] && [ -n "${TSUGUMI_CODESIGN_IDENTITY:-}" ]; then' in script
    assert 'identity="$TSUGUMI_CODESIGN_IDENTITY"' in script
    # the helpers and the app are signed alike, and nothing is signed ad-hoc by a fixed "-"
    assert 'codesign --force --sign "$identity" "$app/Contents/MacOS/tsugumi-hostd"' in script
    assert 'codesign --force --sign "$identity" --entitlements' in script
    assert "--sign -" not in script


def test_paths_of_the_build_machine_stay_in_debug_builds():
    outside = []
    for path in (ROOT / "macOS/Sources").rglob("*.swift"):
        depth = 0  # how many #if DEBUG blocks the line is in (an #else leaves it)
        stack = []
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            s = line.strip()
            if s.startswith("#if"):
                stack.append(s == "#if DEBUG")
            elif s.startswith("#else") and stack:
                stack[-1] = False
            elif s.startswith("#endif") and stack:
                stack.pop()
            elif "#filePath" in line and not any(stack) and not s.startswith("//"):
                outside.append(f"{path.relative_to(ROOT)}:{n}")
    assert not outside


def test_credentials_are_host_only_inputs_and_absent_from_model_selection_and_replies():
    common = (ROOT / "protocol/service_common.fbs").read_text(encoding="utf-8")
    host = (ROOT / "protocol/host.fbs").read_text(encoding="utf-8")
    agent = (ROOT / "protocol/agent.fbs").read_text(encoding="utf-8")
    selection = re.search(r"table ProviderSelection \{([^}]+)\}", common).group(1)
    assert "profile:string (required)" in selection
    for secret in ("api_key", "access_token", "refresh_token", "credential", "endpoint"):
        assert secret not in selection
    assert "api_key:string" in re.search(r"table ProviderInput \{([^}]+)\}", host).group(1)
    assert "provider:ProviderInput" in host
    assert "ProviderInput" not in common and "ProviderInput" not in agent
    assert "provider:Tsugumi.Service.ProviderSelection" in agent
    replies = re.search(r"union ReplyPayload \{([^}]+)\}", host).group(1)
    assert "ProviderInput" not in replies and "CredentialToken" not in replies
    assert "CredentialTokenRequest" not in host


def test_the_vm_gets_every_source_the_core_package_reads():
    """The VM builds the core from the paths in vm-source.txt; a path nix/core.nix
    reads that is not among them fails only on a real first boot."""
    import re
    shared = (ROOT / "macOS/Container/vm-source.txt").read_text().split()
    shared = [entry for entry in shared if not entry.startswith("#")]
    read = set(re.findall(r"\.\./([A-Za-z0-9_./-]+)", (ROOT / "nix/core.nix").read_text()))
    read.discard(".")  # `root = ../.` is the base the paths are relative to
    missing = sorted(path for path in read
                     if not any(path == entry or path.startswith(entry + "/") for entry in shared))
    assert not missing, f"add to macOS/Container/vm-source.txt: {missing}"


def test_the_core_test_package_gets_every_fixture_its_build_installs():
    """The core test manifests install fixtures from tests/fixtures; one that
    nix/core-tests.nix does not list fails only in the Nix build."""
    import re
    manifests = [ROOT / "tests/core/CMakeLists.txt", *sorted((ROOT / "tests/core/cmake").rglob("*.cmake"))]
    wanted = set(re.findall(r"fixtures/([A-Za-z0-9_.-]+\.json)", "\n".join(path.read_text() for path in manifests)))
    listed = set(re.findall(r"\.\./tests/fixtures/([A-Za-z0-9_.-]+)", (ROOT / "nix/core-tests.nix").read_text()))
    missing = sorted(wanted - listed)
    assert not missing, f"add to nix/core-tests.nix: {missing}"
