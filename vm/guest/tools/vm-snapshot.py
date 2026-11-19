#!/usr/bin/env python3
"""Snapshot the guest's current owning Nix inputs, including uncommitted files."""
import shutil
import subprocess
import sys
from pathlib import Path

repo, stage = map(Path, sys.argv[1:])
source = subprocess.check_output([
    "nix", "build", "--no-link", "--print-out-paths", f"path:{repo.resolve()}#guest-source",
], text=True).strip()
shutil.copytree(source, stage, symlinks=True, dirs_exist_ok=True)
