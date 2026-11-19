"""Check native suite coverage without building or starting test services."""

import ast
import os
import re
import subprocess
from pathlib import Path

import pytest

from core_platforms import is_native_core
from test_ci_selection import ROOT, checks, selection


SUITES = {"agent": "agent-rpc", "core": "rpc", "host": "host"}


@pytest.fixture(scope="module")
def commands():
    plans = {}
    env = {**os.environ, "TSUGUMI_FULL_RULES": "0"}
    for system in selection.SYSTEMS:
        for suite, job in SUITES.items():
            output = subprocess.check_output(
                ["bash", "tests/ci/run-suite.sh", "--list", suite, system], cwd=ROOT, env=env,
            ).decode()
            plans[system, job] = [command.split("\0") for command in output.rstrip("\0").split("\0\0")]
    return plans


def selects_python(command, path, name):
    if command[:3] != ["python3", "-m", "pytest"]:
        return False
    if any(path.is_relative_to(Path(arg.removeprefix("--ignore=")))
           for arg in command if arg.startswith("--ignore=")):
        return False
    if not any(path.is_relative_to(Path(arg)) for arg in command if arg.startswith("tests/")):
        return False
    if "-m" in command[3:]:
        marker = command[command.index("-m", 3) + 1]
        assert marker == "native_core", f"Account for the new core marker {marker!r} in this check"
        return is_native_core(path.as_posix(), name)
    return True


def test_each_core_python_test_has_a_ci_runner_when_its_file_changes(commands):
    missing = []
    files = sorted((ROOT / "tests/core").rglob("test_*.py"))
    files.append(ROOT / "tests/python/test_host_projects_rpc.py")
    for file in files:
        path = file.relative_to(ROOT)
        matrix = checks([path.as_posix()])
        for node in ast.walk(ast.parse(file.read_text())):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name.startswith("test_"):
                if not any(selects_python(command, path, node.name)
                           for (system, job), plan in commands.items() if job in matrix.get(system, ())
                           for command in plan):
                    missing.append(f"{path}::{node.name}")
    assert not missing, f"No CI runner selects: {missing}"


def test_every_core_gtest_target_has_an_unfiltered_ci_run(commands):
    core = ROOT / "tests/core"
    manifests = [core / "CMakeLists.txt", *(core / "cmake").rglob("*.cmake")]
    targets = {target for file in manifests for target in
               re.findall(r"gtest_discover_tests\(\s*(\w+)", file.read_text())}
    matrix = checks(None)
    full_runs = {Path(command[0]).name for (system, job), plan in commands.items() if job in matrix.get(system, ())
                 for command in plan if len(command) == 1}
    vm_workflow = (ROOT / ".github/workflows/vm-image.yml").read_text()
    if "vm" in matrix["aarch64-linux"]:
        full_runs.update(re.findall(r"tsugumi-test-env build-ci/libexec/tsugumi-tests/(\w+)\s*$",
                                    vm_workflow, re.MULTILINE))
    assert targets <= full_runs, f"No unfiltered CI run for: {sorted(targets - full_runs)}"
