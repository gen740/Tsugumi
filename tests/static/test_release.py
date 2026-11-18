"""What a release build is made of: one version number (VERSION) for every
part, and no path of the build machine (#filePath) outside debug builds."""
import pathlib
import re
import os
import plistlib
import shlex
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_every_part_takes_the_version_from_one_file():
    version = (ROOT / "VERSION").read_text().strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version)


@pytest.mark.parametrize('release,identity,expected', [(False, '', '-'), (False, 'Development fixture', 'Development fixture'), (True, 'Development fixture', '-')])
def test_app_packaging_uses_the_version_resources_and_signing_policy(tmp_path, release, identity, expected):
    """Execute the packaging script with build/signing command doubles."""
    root = tmp_path / 'project with spaces'
    def write(name, value='fixture', executable=False):
        file = root / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(value)
        if executable: file.chmod(0o755)
    write('VERSION', (ROOT / 'VERSION').read_text())
    write('ui/macos/build-app.sh', (ROOT / 'ui/macos/build-app.sh').read_text())
    for name in ['vm/guest/build-image.sh', 'hostd/tools/build-macos.sh']:
        write(name, '#!/bin/sh\nexit 0\n', True)
    (root / 'vm/macos/runtime').mkdir(parents=True)
    binaries = root / 'built'
    write('ui/macos/tools/flatc-bin.sh', '#!/bin/sh\nprintf \"%s\\n\" \"' + str(root / 'tools') + '\"\n', True)
    for name in ['built/Tsugumi', 'built/tsugumi-vmhost', 'build-host/hostd/tsugumi-hostd',
                 'build-host/Frameworks/library.dylib', 'build-host/generated/bfbs/agent.bfbs', 'build-host/generated/bfbs/host.bfbs',
                 'build-host/Resources/formatters/formatter', 'ui/macos/Resources/AppIcon.icns',
                 'built/SwiftMath_SwiftMath.bundle/Contents/Resources/mathFonts.bundle/latinmodern-math.otf',
                 'ui/macos/Resources/Fonts/latin-modern-math/latinmodern-math.otf',
                 'ui/macos/Resources/Fonts/latin-modern-math/GUST-FONT-LICENSE.txt',
                 'ui/shared/modules/viewer.html', 'ui/shared/modules/viewer.js',
                 'dist/tsugumi-vm/tsugumi-vm.tar.gz', 'dist/tsugumi-vm/vm-manifest.json', 'demos/fixture/tsugumi.json',
                 'ui/macos/third-party/LICENSE', 'third-party/LICENSE', 'hostd/formatters/third-party/LICENSE',
                 'ui/macos/Sources/Grammars/TreeSitterNix/LICENSE', 'ui/macos/Sources/Grammars/TreeSitterPython/LICENSE',
                 'ui/macos/Sources/Grammars/TreeSitterJuliaScanner/LICENSE', 'ui/macos/Sources/Grammars/LICENSE.tree-sitter',
                 'ui/macos/Resources/ja.lproj/Localizable.strings', 'ui/macos/Resources/en.lproj/Localizable.strings']:
        write(name)
    write('ui/macos/Resources/tools/en_strings.py', '')
    recording = tmp_path / 'signing'
    # Absolute OS commands are doubles too: no app is built, signed or registered.
    shell = r'''function /usr/bin/strip() { :; }
function /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister() { :; }
function strings() { :; }
function env() { case "$*" in *--show-bin-path*) printf '%s\n' "$FIXTURE_BIN" ;; esac; }
function codesign() { printf '%q ' "$@" >> "$SIGN_LOG"; printf '\n' >> "$SIGN_LOG"; }
script=$1
shift
source "$script"
'''
    environment = dict(os.environ, TSUGUMI_CODESIGN_IDENTITY=identity, FIXTURE_BIN=str(binaries), SIGN_LOG=str(recording))
    command = ['bash', '-c', shell, str(root / 'ui/macos/build-app.sh'), str(root / 'ui/macos/build-app.sh')]
    if release: command.append('--release')
    result = subprocess.run(command, env=environment, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    app = root / 'dist/Tsugumi.app'
    with (app / 'Contents/Info.plist').open('rb') as file: metadata = plistlib.load(file)
    assert metadata['CFBundleShortVersionString'] == (ROOT / 'VERSION').read_text().strip()
    resources = app / 'Contents/Resources'
    assert (resources / 'modules/viewer.js').read_text() == 'fixture'
    assert (resources / 'tsugumi-vm/tsugumi-vm.tar.gz').read_text() == 'fixture'
    assert (resources / 'SwiftMath_SwiftMath.bundle/Contents/Resources/mathFonts.bundle/latinmodern-math.otf').is_file()
    signed = [shlex.split(line) for line in recording.read_text().splitlines()]
    assert all(args[args.index('--sign') + 1] == expected for args in signed)
    assert {str(app), str(app / 'Contents/MacOS/tsugumi-hostd'), str(app / 'Contents/MacOS/tsugumi-vmhost')} <= {args[-1] for args in signed}


def test_paths_of_the_build_machine_stay_in_debug_builds():
    outside = []
    roots = [ROOT / path for path in ("ui/macos/Sources", "vm/macos/runtime/Sources", "client/swift/Sources")]
    sources = []
    for root in roots:
        owned = list(root.rglob("*.swift"))
        assert owned, f"No Swift sources in {root}"
        sources.extend(owned)
    for path in sources:
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


def schema_tables(source):
    """Read table names, field types and attributes from these FlatBuffers schemas."""
    source = re.sub(r"//[^\n]*", "", source)
    tables = {}
    for name, body in re.findall(r"\btable\s+(\w+)\s*\{([^}]+)\}", source):
        fields = {}
        for declaration in body.split(";"):
            field = re.match(r"\s*(\w+)\s*:\s*([\w.]+|\[[\w.]+\])", declaration)
            if field:
                attributes = re.findall(r"\(([^)]+)\)", declaration)
                fields[field[1]] = (field[2], {a.strip() for group in attributes for a in group.split(",")})
        tables[name] = fields
    return tables


def test_credentials_are_host_only_inputs_and_absent_from_model_selection_and_replies():
    common = schema_tables((ROOT / "protocol/service_common.fbs").read_text())
    host_source = (ROOT / "protocol/host.fbs").read_text()
    host = schema_tables(host_source)
    agent = schema_tables((ROOT / "protocol/agent.fbs").read_text())
    selection = common["ProviderSelection"]
    profile_type, profile_attributes = selection["profile"]
    assert profile_type == "string" and "required" in profile_attributes
    assert not selection.keys() & {"api_key", "access_token", "refresh_token", "credential", "endpoint"}
    assert host["ProviderInput"]["api_key"][0] == "string"
    assert host["ProviderSetRequest"]["provider"][0] == "ProviderInput"
    assert agent["ConfigureRequest"]["provider"][0] == "Tsugumi.Service.ProviderSelection"
    assert all(kind.split(".")[-1] != "ProviderInput"
               for fields in [*common.values(), *agent.values()] for kind, _ in fields.values())
    reply = re.search(r"\bunion\s+ReplyPayload\s*\{([^}]+)\}", host_source)[1]
    reply = re.sub(r"//[^\n]*", "", reply)
    reply_types = {variant.partition("=")[0].split(":")[-1].strip().rsplit(".", 1)[-1]
                   for variant in reply.split(",")}
    assert not reply_types & {"ProviderInput", "CredentialToken"}
    assert "CredentialTokenRequest" not in host


def test_the_core_test_package_gets_every_fixture_its_build_installs():
    """The core test manifests install fixtures from tests/fixtures; one that
    nix/core-tests.nix does not list fails only in the Nix build."""
    import re
    manifests = [ROOT / "tests/core/CMakeLists.txt", *sorted((ROOT / "tests/core/cmake").rglob("*.cmake"))]
    wanted = set(re.findall(r"fixtures/([A-Za-z0-9_.-]+\.json)", "\n".join(path.read_text() for path in manifests)))
    listed = set(re.findall(r"\.\./tests/fixtures/([A-Za-z0-9_.-]+)", (ROOT / "nix/core-tests.nix").read_text()))
    missing = sorted(wanted - listed)
    assert not missing, f"add to nix/core-tests.nix: {missing}"
    installed = ROOT / "build-ci/share/tsugumi/guide-tests/tests/fixtures"
    if installed.is_dir():
        assert wanted <= {entry.name for entry in installed.iterdir() if entry.is_file()}
