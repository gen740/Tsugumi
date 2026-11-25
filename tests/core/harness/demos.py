"""Access the pinned demo source and make writable project copies."""

import os
from pathlib import Path
import shutil
import stat


def demos_dir():
    if not (root := os.environ.get("TSUGUMI_DEMOS_DIR")):
        raise RuntimeError("Set TSUGUMI_DEMOS_DIR to the pinned demos tree (provided by nix develop)")
    return Path(root)


def copy_demo(language, name, destination, **options):
    shutil.copytree(demos_dir() / language / name, destination, **options)
    for path in [destination, *destination.rglob("*")]:
        if not path.is_symlink():
            path.chmod(path.stat().st_mode | stat.S_IWUSR)
