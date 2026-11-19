"""Release guest selection and protocol credential boundaries."""
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_app_packaging_selects_the_prebuilt_release_guest(tmp_path):
    """Capture the guest request before any build or signing steps."""
    script = tmp_path / "ui/macos/build-app.sh"
    script.parent.mkdir(parents=True)
    script.write_bytes((ROOT / "ui/macos/build-app.sh").read_bytes())
    (tmp_path / "VERSION").write_bytes((ROOT / "VERSION").read_bytes())
    shell = r"""
        nix() { printf '%s\n' "$@"; exit 42; }
        source "$0"
    """
    result = subprocess.run(["bash", "-c", shell, str(script), "--release"],
                            cwd=tmp_path, capture_output=True, text=True)
    assert result.returncode == 42, result.stderr
    assert result.stdout.splitlines() == ["run", ".#guest-image", "--", "--release"]


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
