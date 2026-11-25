"""Read the FlatBuffers control files of a test-owned project daemon."""

from Tsugumi.State.ServerReady import ServerReady
from Tsugumi.State.StartupError import StartupError


def _text(value):
    return value.decode() if value else ""


def read_ready(path):
    ready = ServerReady.GetRootAs(path.read_bytes())
    return {
        "endpoint": _text(ready.Endpoint()), "project_id": _text(ready.ProjectId()),
        "token": _text(ready.Token()), "unix_socket": _text(ready.UnixSocket()),
        "pid": ready.Pid(), "protocol": ready.Protocol(),
    }


def read_startup_error(path):
    error = StartupError.GetRootAs(path.read_bytes())
    return {
        "kind": _text(error.Kind()), "found": _text(error.Found()),
        "expected": _text(error.Expected()), "source": _text(error.Source()),
        "message": _text(error.Message()), "path": _text(error.Path()),
    }
