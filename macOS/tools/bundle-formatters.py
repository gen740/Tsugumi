#!/usr/bin/env python3
"""Bundle pinned Mach-O formatter tools and relocate their Nix dylib closure."""
from pathlib import Path
import shutil
import subprocess
import sys

out = Path(sys.argv[1])
if out.exists():
    shutil.rmtree(out)
out.mkdir(parents=True)
copied = {}


def bundle(source, name):
    source = Path(source).resolve()
    if source in copied:
        return copied[source]
    target = out / name
    copied[source] = target
    shutil.copy2(source, target)
    target.chmod(0o755)
    dependencies = subprocess.check_output(['/usr/bin/otool', '-L', str(source)], text=True)
    for line in dependencies.splitlines()[1:]:
        dependency = line.strip().split(' (', 1)[0]
        if not dependency.startswith('/nix/store/') or Path(dependency).resolve() == source:
            continue
        # Include the store hash to keep equally named libraries distinct.
        libname = Path(dependency).parts[3].split('-', 1)[0] + '-' + Path(dependency).name
        library = bundle(dependency, libname)
        subprocess.run(['/usr/bin/install_name_tool', '-change', dependency,
                        '@loader_path/' + library.name, str(target)], check=True)
    if source.suffix == '.dylib':
        subprocess.run(['/usr/bin/install_name_tool', '-id', '@loader_path/' + target.name, str(target)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(target)], check=True)
    return target


for source, name in zip(sys.argv[2:], ['clang-format', 'ruff', 'nixfmt', 'shfmt'], strict=True):
    bundle(source, name)
