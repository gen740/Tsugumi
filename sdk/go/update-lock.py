#!/usr/bin/env python3
"""Update a Card's go.mod/go.sum without leaving generated SDK files in src."""
import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('source',type=Path,help='the Card src directory containing go.mod')
parser.add_argument('--offline',action='store_true')
args=parser.parse_args()
source=args.source.resolve()
sdk=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='tsugumi-go-') as tmp:
    copy=Path(tmp)/'card'
    shutil.copytree(source,copy,ignore=shutil.ignore_patterns('tsugumi','tsugumi_adapter.go'))
    generated=copy/'tsugumi';generated.mkdir()
    shutil.copyfile(sdk/'go.mod',generated/'go.mod')
    (generated/'sdk.go').write_text('package tsugumi\n')
    env={**os.environ,'GOTOOLCHAIN':'local'}
    if args.offline:env.update(GOPROXY='off',GOSUMDB='off')
    # go list parses imports even when a generated nested message package is
    # absent. Stub only those local SDK package names; tidy does not typecheck.
    listing=subprocess.run(['go','list','-e','-json','./...'],cwd=copy,env=env,
                           check=True,capture_output=True,text=True).stdout
    decoder=json.JSONDecoder()
    while listing.strip():
        package,end=decoder.raw_decode(listing.lstrip())
        listing=listing.lstrip()[end:]
        for imported in package.get('Imports',[]):
            if not imported.startswith('tsugumi/'):
                continue
            parts=imported.split('/')[1:]
            if any(not part.replace('_','').isalnum() for part in parts):
                raise SystemExit(f'Invalid generated SDK import: {imported}')
            directory=generated.joinpath(*parts)
            directory.mkdir(parents=True,exist_ok=True)
            (directory/'sdk.go').write_text('package generated\n')
    subprocess.run(['go','mod','tidy'],cwd=copy,env=env,check=True)
    for name in ('go.mod','go.sum'):
        path=copy/name
        (source/name).write_text(path.read_text() if path.exists() else '')
