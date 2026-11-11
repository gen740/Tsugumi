"""Inject artifact corruption without exposing a storage layout to test cases."""

import os
import stat
from pathlib import Path


def corrupt_retained_file(project: Path, original: bytes) -> None:
    """Replace copies of a unique fixture output after the execution cache is cleared.

    Match complete content rather than filenames, database rows or directory names.
    The worker's scenario lives outside the project, so only its saved output has
    these exact bytes. Never follow a symlink outside the fixture's project.
    """
    if not original:
        raise ValueError("artifact fault injection requires unique nonempty content")
    matches = []
    for directory, _, filenames in os.walk(project, followlinks=False):
        for filename in filenames:
            candidate = Path(directory) / filename
            info = candidate.lstat()
            if stat.S_ISREG(info.st_mode) and info.st_size == len(original) and candidate.read_bytes() == original:
                matches.append((candidate, stat.S_IMODE(info.st_mode)))
    if not matches:
        raise RuntimeError("no retained copy of the fixture output was found for corruption")
    for candidate, mode in matches:
        candidate.chmod(mode | stat.S_IWUSR)
        try:
            candidate.write_bytes(original + b"\ncorrupted after retention\n")
        finally:
            candidate.chmod(mode)
