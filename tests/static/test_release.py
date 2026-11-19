"""Release version, credential boundaries and required test fixtures."""
import pathlib
import re
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_every_part_takes_the_version_from_one_file():
    version = (ROOT / "VERSION").read_text().strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version)


@pytest.mark.parametrize("flags", [[], ["--release"]], ids=["development", "release"])
def test_app_packaging_selects_the_prebuilt_guest(tmp_path, flags):
    """Capture the guest request before any build or signing steps."""
    script = tmp_path / "ui/macos/build-app.sh"
    script.parent.mkdir(parents=True)
    script.write_bytes((ROOT / "ui/macos/build-app.sh").read_bytes())
    (tmp_path / "VERSION").write_bytes((ROOT / "VERSION").read_bytes())
    shell = r"""
        nix() { printf '%s\n' "$@"; exit 42; }
        script=$1
        shift
        source "$script"
    """
    result = subprocess.run(["bash", "-c", shell, "fixture", str(script), *flags],
                            cwd=tmp_path, capture_output=True, text=True)
    assert result.returncode == 42, result.stderr
    assert result.stdout.splitlines() == ["run", ".#guest-image", "--", *flags]


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
import subprocess

import pytest
    manifests = [ROOT / "tests/core/CMakeLists.txt", *sorted((ROOT / "tests/core/cmake").rglob("*.cmake"))]
    wanted = set(re.findall(r"fixtures/([A-Za-z0-9_.-]+\.json)", "\n".join(path.read_text() for path in manifests)))
    listed = set(re.findall(r"\.\./tests/fixtures/([A-Za-z0-9_.-]+)", (ROOT / "nix/core-tests.nix").read_text()))
    missing = sorted(wanted - listed)
    assert not missing, f"add to nix/core-tests.nix: {missing}"
    installed = ROOT / "build-ci/share/tsugumi/guide-tests/tests/fixtures"
    if installed.is_dir():
        assert wanted <= {entry.name for entry in installed.iterdir() if entry.is_file()}
