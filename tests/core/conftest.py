"""Each case owns its daemon, clients, external doubles and their cleanup."""

from contextlib import ExitStack
import inspect
import re

import pytest

from harness.daemon import Daemon, write_project
from harness.scenario import worker_launcher


@pytest.fixture
def project(tmp_path):
    root = tmp_path / "project"
    write_project(root)
    return root


@pytest.fixture
def core_factory(project, tmp_path, request):
    with ExitStack() as stack:
        daemons = []
        def create(*, project_root=None, mocked_builds=False, mocked_worker=False,
                   fault=None, history_dir=None, args=None):
            worker = worker_launcher(tmp_path) if mocked_worker else None
            d = Daemon(project_root or project, mocked_builds=mocked_builds, worker=worker,
                       fault=fault, history_dir=history_dir, args=args)
            stack.callback(d.close)
            daemons.append(d)
            d.start()
            return d
        yield create
        for index, daemon in enumerate(daemons):
            request.node.add_report_section("teardown", f"daemon {index + 1}", daemon.diagnostics())


@pytest.fixture
def daemon(core_factory):
    return core_factory()


@pytest.fixture
def client(daemon):
    return daemon.connect()


def pytest_addoption(parser):
    parser.addoption("--core-report", action="store_true", help="Show core scenarios as readable behavior statements")


def pytest_configure(config):
    config.addinivalue_line("markers", "environment: uses real Nix packages, networking or host sandbox facilities")


def pytest_collection_modifyitems(items):
    for item in items:
        if "tests/core/integration/" in str(item.path):
            item.add_marker(pytest.mark.environment)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    if "tests/core/" not in str(item.path):
        return
    description = inspect.getdoc(item.obj)
    if description:
        paragraph = " ".join(description.split("\n\n", 1)[0].split())
        report.core_description = re.split(r"(?<=[.!?])\s+", paragraph, maxsplit=1)[0]
    else:
        report.core_description = item.originalname.removeprefix("test_").replace("_", " ")
    if hasattr(item, "callspec"):
        report.core_description += f" [{item.callspec.id}]"


def pytest_terminal_summary(terminalreporter, exitstatus, config):
    if not config.getoption("--core-report"):
        return
    terminalreporter.section("Core RPC behaviors")
    for category, label in [("passed", "PASS"), ("xfailed", "KNOWN BUG"),
                            ("failed", "FAIL"), ("error", "ERROR"),
                            ("xpassed", "FIXED?"), ("skipped", "SKIP")]:
        for report in terminalreporter.stats.get(category, []):
            description = getattr(report, "core_description", None)
            if description:
                terminalreporter.write_line(f"{label}: {description}")
                if getattr(report, "wasxfail", None):
                    terminalreporter.write_line(f"  {report.wasxfail}")
