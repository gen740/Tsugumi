"""What a release build is made of: one version number (VERSION) for every
part, and no path of the build machine (#filePath) outside debug builds."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_every_part_takes_the_version_from_one_file():
    version = (ROOT / "VERSION").read_text().strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version)
    assert 'file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TSUGUMI_VERSION' in (ROOT / "CMakeLists.txt").read_text()
    assert "version = lib.fileContents ../VERSION;" in (ROOT / "nix/core.nix").read_text()
    assert "VERSION" in (ROOT / "macOS/Container/vm-source.txt").read_text().split()
    assert "VERSION" in (ROOT / "macOS/build-app.sh").read_text()


def test_paths_of_the_build_machine_stay_in_debug_builds():
    outside = []
    for path in list((ROOT / "macOS/Sources").rglob("*.swift")) + list((ROOT / "agent/Sources").rglob("*.swift")):
        depth = 0  # how many #if DEBUG blocks the line is in (an #else leaves it)
        stack = []
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            s = line.strip()
            if s.startswith("#if"):
                stack.append(s == "#if DEBUG")
            elif s.startswith("#else") and stack:
                stack[-1] = False
            elif s.startswith("#endif") and stack:
                stack.pop()
            elif "#filePath" in line and not any(stack) and not s.startswith("//"):
                outside.append(f"{path.relative_to(ROOT)}:{n}")
    assert not outside
