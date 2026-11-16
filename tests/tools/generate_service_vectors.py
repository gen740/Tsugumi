"""Regenerate shared C++/Swift/Web service wire goldens with flatc.

The fixture keeps the inputs and expected presentation values. Only wire bytes
and the schema-derived external Agent request list are regenerated here.
"""
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/service-contract.json"


def main():
    flatc = shutil.which("flatc")
    if not flatc:
        raise SystemExit("flatc is required")
    fixture = json.loads(FIXTURE.read_text())
    with tempfile.TemporaryDirectory() as temporary:
        source = Path(temporary) / "vector.json"
        for group, envelope in [("requests", "Request"), ("replies", "Reply")]:
            for case in fixture[group]:
                schema = "host" if case["host"] else "agent"
                name = case["method"] if group == "requests" else case["type"]
                value = case["fields"]
                source.write_text(json.dumps({
                    "request_id": 7,
                    "payload_type": envelope,
                    "payload": {"value_type": name, "value": value},
                }))
                subprocess.run([
                    flatc, "-b", "--strict-json", "-I", str(ROOT / "protocol"),
                    "-o", temporary, str(ROOT / f"protocol/{schema}.fbs"), str(source),
                ], check=True)
                case["hex"] = source.with_suffix(".bin").read_bytes().hex()
    schema = (ROOT / "protocol/agent.fbs").read_text()
    members = re.search(r"union RequestPayload \{(.*?)\}", schema, re.S).group(1)
    fixture["agentRequests"] = [
        member.strip().split(":")[0] for member in members.split(",")
        if member.strip() and member.strip() not in ("HelloRequest", "ToolCallCancelRequest")
    ]
    FIXTURE.write_text(json.dumps(fixture, indent=2) + "\n")


if __name__ == "__main__":
    main()
