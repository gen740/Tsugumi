"""Keep runtime package data and discovery metadata in installed Card dependencies."""
import csv
from email import policy
from email.parser import BytesParser
from pathlib import Path
import re
import shutil
import sys


def trim(root):
    root = Path(root)
    directories = {'doc', 'docs', 'example', 'examples', 'test', 'tests', 'spec', 'specs', 'licenses'}
    extra = re.compile(r'^(license|notice|copying|copyright|authors|readme|changelog|changes|news)([._-].*|v[0-9].*)?$', re.I)
    source = {'.py', '.rb', '.js', '.mjs', '.json', '.so', '.dll', '.dylib', '.r', '.jl',
              '.cs', '.go', '.rs', '.wasm', '.h', '.hh', '.c', '.cc'}
    for path in sorted(root.rglob('*'), key=lambda p: len(p.parts), reverse=True):
        if path.is_dir() and (path.name.lower() in directories or path.suffix == '.dSYM'):
            if path.is_symlink():
                path.unlink()
            else:
                shutil.rmtree(path)
        elif path.is_file() and ((extra.fullmatch(path.name) and path.suffix.lower() not in source)
                                 or path.suffix in {'.map', '.pdb', '.debug'}):
            path.unlink()

    headers = {'metadata-version', 'name', 'version', 'requires-python', 'requires-dist',
               'provides-extra', 'requires-external', 'import-name', 'import-namespace',
               'provides-dist', 'obsoletes-dist'}
    for directory in root.rglob('*.dist-info'):
        metadata = directory / 'METADATA'
        message = BytesParser(policy=policy.compat32).parsebytes(metadata.read_bytes())
        for name in list(message.keys()):
            if name.lower() not in headers:
                del message[name]
        message.set_payload(None)
        metadata.write_bytes(message.as_bytes())
        for name in ('WHEEL', 'INSTALLER', 'REQUESTED', 'direct_url.json'):
            (directory / name).unlink(missing_ok=True)
        record = directory / 'RECORD'
        if record.exists():
            with record.open(newline='') as stream:
                rows = list(csv.reader(stream))
            with record.open('w', newline='') as stream:
                writer = csv.writer(stream)
                for row in rows:
                    path = directory.parent / row[0]
                    if path.is_file():
                        if path == metadata:
                            row[1:] = ['', str(metadata.stat().st_size)]
                        elif path == record:
                            row[1:] = ['', '']
                        writer.writerow(row)


if __name__ == '__main__':
    trim(sys.argv[1])
