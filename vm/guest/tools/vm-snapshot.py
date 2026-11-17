#!/usr/bin/env python3
"""Snapshot Git's tracked and nonignored untracked build inputs, without an index mutation."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

repo, stage = map(Path, sys.argv[1:])
stage.mkdir(parents=True, exist_ok=True)
for entry in set(subprocess.check_output(["git", "-C", str(repo), "ls-files", "-z", "-co", "--exclude-standard"]).split(b"\0")):
    if not entry:
        continue
    name = os.fsdecode(entry)
    src, dst = repo / name, stage / name
    if not src.exists() and not src.is_symlink():
        continue
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst, follow_symlinks=False)
    if not src.is_symlink():
        shutil.copymode(src, dst)
