"""Shared public-RPC support for core and remaining integration suites."""

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent / "core"))


def pytest_configure(config):
    # Apply also to direct pytest invocations and all spawned Web/MCP servers.
    os.environ['TSUGUMI_TEST_NO_SECRET_STORE'] = '1'
    if 'TSUGUMI_TEST_RUN_ROOT' not in os.environ:
        pytest.exit('Run pytest through tsugumi-test-env or tests/run-isolated.py', returncode=4)
    for marker in (
        "shared_rules: platform-neutral document, validation, layout and version matrices",
        "sdk_pure: Python SDK library semantics owned by the canonical Linux job",
        "package_smoke: actual packaged client/server or native library smoke on every architecture",
    ):
        config.addinivalue_line("markers", marker)
    config.addinivalue_line(
        "markers",
        "static: reads only files of the repository (or runs a generator with --check); needs no daemon, "
        "no Nix build of the project and no network. Everything under tests/static is marked so. CI runs "
        "tests/static first, once, in the Static checks job; the heavy jobs run -m 'not static'",
    )


# Explicit shared semantic matrices. Transport, crash recovery, filesystem,
# worker/shell/sandbox and real environment tests are deliberately absent.
SHARED_RULE_FILES = {
    "tests/core/test_rpc_schema.py", "tests/core/test_graph.py",
    "tests/core/test_definitions.py", "tests/core/test_rpc_values.py",
    "tests/core/test_settings_exclusion.py",
    "tests/core/test_meta_opaque.py", "tests/core/test_versions.py",
    "tests/core/test_rpc_drafts.py",
}
SHARED_RULE_CASES = {
    "test_operation_conflict_and_required_fields", "test_document_validation",
    "test_view_save_cas", "test_validate_revision", "test_ui_layout_fields_and_draft_outputs",
    "test_uint64_max_and_original_spelling_preserved", "test_explicit_false_is_not_omitted",
    "test_an_empty_optional_document_is_not_given", "test_draft_resolution_ignores_meta_schema_and_layout",
}
SDK_PURE_FILES = {
    "test_sdk_roles.py", "test_mpl_backend.py", "test_plots.py",
    "test_visualization.py", "test_notebook.py",
}
PACKAGE_SMOKES = {
    # A real packaged server and Wasm policy, plus the packaged CLI entry point.
    "test_only_the_page_with_the_key_reaches_the_api",
    "test_the_shared_wasm_client_is_served_and_allowed_by_the_page_policy",
    "test_a_second_run_opens_a_page_on_the_running_server",
    # Native imports/rendering remain on the second SDK architecture.
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
        if path in SHARED_RULE_FILES or (path == "tests/core/test_contract.py" and name in SHARED_RULE_CASES):
            item.add_marker(pytest.mark.shared_rules)
        if path.startswith("tests/python/") and item.path.name in SDK_PURE_FILES:
            item.add_marker(pytest.mark.sdk_pure)
        if name in PACKAGE_SMOKES:
            item.add_marker(pytest.mark.package_smoke)
