#!/usr/bin/env python3
"""Download the Actions artifact of an evaluated guest identity (gh auth required).

The artifact is named after the identity and uploaded only after its job built
and verified the image, so any unexpired one will do, whatever the other jobs
of its run did; the caller verifies it again against the identity."""
import subprocess
import sys
from pathlib import Path
import json

identity, destination, repository = sys.argv[1:]
name = f"vm-{identity}"
result = subprocess.run(["gh", "api", "--paginate", "--slurp",
                         f"repos/{repository}/actions/artifacts?name={name}&per_page=100"],
                        capture_output=True, text=True, check=True)
for page in json.loads(result.stdout):
    for artifact in page["artifacts"]:
        if artifact["expired"] or artifact["name"] != name:
            continue
        run = artifact["workflow_run"]["id"]
        subprocess.run(["gh", "run", "download", str(run), "--repo", repository, "--name", name,
                        "--dir", str(Path(destination).resolve())], check=True)
        sys.exit(0)
sys.exit(1)
