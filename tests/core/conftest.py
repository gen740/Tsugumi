"""Each case owns its daemon, clients, external doubles and their cleanup."""

from contextlib import ExitStack

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
                   fault=None, args=None):
            worker = worker_launcher(tmp_path) if mocked_worker else None
            d = Daemon(project_root or project, mocked_builds=mocked_builds, worker=worker,
                       fault=fault, args=args)
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



def pytest_configure(config):
    config.addinivalue_line("markers", "environment: uses real Nix packages, networking or host sandbox facilities")


def pytest_collection_modifyitems(items):
    for item in items:
        if "tests/core/integration/" in str(item.path):
            item.add_marker(pytest.mark.environment)
