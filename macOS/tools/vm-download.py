#!/usr/bin/env python3
"""Find a successful Actions artifact for an evaluated guest identity (gh auth required)."""
import json
import subprocess
import sys
from pathlib import Path

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
        status = json.loads(subprocess.check_output(["gh", "api", f"repos/{repository}/actions/runs/{run}"], text=True))
        if status["conclusion"] != "success":
            continue
        subprocess.run(["gh", "run", "download", str(run), "--repo", repository, "--name", name,
                        "--dir", str(Path(destination).resolve())], check=True)
        sys.exit(0)
sys.exit(1)
