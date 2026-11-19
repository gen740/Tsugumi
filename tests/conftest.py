"""Shared public-RPC support for core and remaining integration suites."""

import os
import sys
from pathlib import Path

import pytest

sys.path[:0] = [str(Path(__file__).parent / "core"), str(Path(__file__).parent / "ci")]

from core_platforms import is_native_core


def pytest_configure(config):
    # Apply also to direct pytest invocations and all spawned Web/MCP servers.
    os.environ['TSUGUMI_TEST_NO_SECRET_STORE'] = '1'
    if 'TSUGUMI_TEST_RUN_ROOT' not in os.environ:
        pytest.exit('Run pytest through tsugumi-test-env or tests/run-isolated.py', returncode=4)
    for marker in (
        "native_core: exercises native transport, processes, filesystem or system selection",
        "sdk_pure: Python SDK library semantics owned by the canonical Linux job",
        "package_smoke: exercises a packaged client/server or native library",
    ):
        config.addinivalue_line("markers", marker)
    config.addinivalue_line(
        "markers",
        "static: reads only files of the repository (or runs a generator with --check); needs no daemon, "
        "no Nix build of the project and no network. Everything under tests/static is marked so. CI runs "
        "tests/static once, in the Static checks job",
    )


SDK_PURE_FILES = {
    "test_sdk_roles.py", "test_mpl_backend.py", "test_plots.py",
    "test_visualization.py", "test_notebook.py",
}
PACKAGE_SMOKES = {
    # A real packaged server and Wasm policy, plus the packaged CLI entry point.
    "test_only_the_page_with_the_key_reaches_the_api",
    "test_the_shared_wasm_client_is_served_and_allowed_by_the_page_policy",
    "test_a_second_run_opens_a_page_on_the_running_server",
    # Exercise native imports and rendering in the filtered SDK run.
    "test_report_figure_sends_plotly",
    "test_the_backend_is_chosen_and_show_sends_one_image",
    "test_notebook_passes_typed_result_and_keeps_snapshot",
    "test_notebook_child_is_reaped_on_cancel_or_deadline",
    "test_cell_failure_exposes_notebook_source_and_no_result",
}


def pytest_collection_modifyitems(items):
    for item in items:
        path = item.path.relative_to(Path(__file__).resolve().parents[1]).as_posix()
        name = getattr(item, "originalname", item.name)
        if path.startswith("tests/static/"):
            item.add_marker(pytest.mark.static)
        if is_native_core(path, name):
            item.add_marker(pytest.mark.native_core)
        if path.startswith("tests/python/") and item.path.name in SDK_PURE_FILES:
            item.add_marker(pytest.mark.sdk_pure)
        if name in PACKAGE_SMOKES:
            item.add_marker(pytest.mark.package_smoke)
