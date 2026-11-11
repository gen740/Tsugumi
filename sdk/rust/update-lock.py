#!/usr/bin/env python3
"""Resolve a Card's Cargo.lock without keeping a generated SDK in its source."""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path, help='the Card src directory containing Cargo.toml')
parser.add_argument('--offline', action='store_true')
args = parser.parse_args()
source = args.source.resolve()
sdk = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='tsugumi-cargo-') as tmp:
    copy = Path(tmp) / 'card'
    shutil.copytree(source, copy, ignore=shutil.ignore_patterns('target', 'tsugumi'))
    generated = copy / 'tsugumi'
    (generated / 'src').mkdir(parents=True)
    shutil.copyfile(sdk / 'Cargo.toml', generated / 'Cargo.toml')
    # Cargo resolves the SDK's dependencies without compiling its generated face.
    (generated / 'src/lib.rs').write_text('')
    subprocess.run(['cargo', 'generate-lockfile', *(['--offline'] if args.offline else [])], cwd=copy, check=True)
    shutil.copyfile(copy / 'Cargo.lock', source / 'Cargo.lock')
