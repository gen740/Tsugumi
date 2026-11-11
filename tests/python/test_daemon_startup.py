"""Process startup behavior of the daemon executable."""

from conftest import Daemon


def test_second_daemon_is_refused(daemon):
    """A second daemon cannot take ownership of an already open project."""
    second = Daemon(daemon.project)
    try:
        second.start(expect_exit=True)
        assert second.proc.wait(timeout=10) == 4
    finally:
        second.cleanup()
