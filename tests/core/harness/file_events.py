"""Wait for fixture files and process exit using kernel notifications."""

from __future__ import annotations

import ctypes
import os
import select
from contextlib import closing, contextmanager, nullcontext


@contextmanager
def changes(path):
    """Watch a file or directory; the caller must recheck after subscribing."""
    if hasattr(select, "kqueue"):
        descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        try:
            with closing(select.kqueue()) as queue:
                flags = (select.KQ_NOTE_WRITE | select.KQ_NOTE_EXTEND |
                         select.KQ_NOTE_RENAME | select.KQ_NOTE_DELETE)
                queue.control([select.kevent(descriptor, filter=select.KQ_FILTER_VNODE,
                                            flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
                                            fflags=flags)], 0)
                yield queue.fileno()
        finally:
            os.close(descriptor)
    else:
        libc = ctypes.CDLL(None, use_errno=True)
        descriptor = libc.inotify_init1(os.O_CLOEXEC)
        if descriptor < 0:
            raise OSError(ctypes.get_errno(), "inotify_init1")
        try:
            modify, close_write = 0x002, 0x008
            moved_from, moved_to = 0x040, 0x080
            create, delete, delete_self, move_self = 0x100, 0x200, 0x400, 0x800
            events = modify | close_write | moved_from | moved_to | create | delete | delete_self | move_self
            if libc.inotify_add_watch(descriptor, os.fsencode(path), events) < 0:
                raise OSError(ctypes.get_errno(), str(path))
            yield descriptor
        finally:
            os.close(descriptor)


@contextmanager
def process_exit(pid):
    if hasattr(select, "kqueue"):
        with closing(select.kqueue()) as queue:
            queue.control([select.kevent(pid, filter=select.KQ_FILTER_PROC,
                                        flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                                        fflags=select.KQ_NOTE_EXIT)], 0)
            yield queue.fileno()
    else:
        descriptor = os.pidfd_open(pid)
        try:
            yield descriptor
        finally:
            os.close(descriptor)


def wait_for_exit(pid):
    try:
        with process_exit(pid) as exited:
            select.select([exited], [], [])
    except ProcessLookupError:
        pass  # The process exited before the subscription.


def terminate_process(pid, grace=5):
    """Let a fixture shut down gracefully before escalating to KILL."""
    import signal

    try:
        with process_exit(pid) as exited:
            os.kill(pid, signal.SIGTERM)
            if not select.select([exited], [], [], grace)[0]:
                os.kill(pid, signal.SIGKILL)
                select.select([exited], [], [])
    except ProcessLookupError:
        pass


def watchable(path):
    while not path.exists():
        path = path.parent
    return path


def wait_for_file(path, read=None, *, exited=None):
    """Wait for appearance, or for a reader to accept the file contents."""
    if read is None:
        read = lambda: path if path.exists() else None
    result = read()
    if result is not None:
        return result
    while True:
        parent = watchable(path.parent)
        existing_file = path.exists()
        try:
            with changes(parent) as directory:
                with changes(path) if existing_file else nullcontext() as file:
                    result = read()
                    if result is not None:
                        return result
                    # A more specific directory or the file itself appeared
                    # during registration. Subscribe to it before blocking.
                    if watchable(path.parent) != parent or path.exists() != (file is not None):
                        continue
                    descriptors = [fd for fd in (directory, file, exited) if fd is not None]
                    readable, _, _ = select.select(descriptors, [], [])
                    if exited is not None and exited in readable:
                        return read()
        except FileNotFoundError:
            if exited is not None and select.select([exited], [], [], 0)[0]:
                return read()


def process_file(process, path, read=None):
    try:
        with process_exit(process.pid) as exited:
            return wait_for_file(path, read, exited=exited)
    except ProcessLookupError:
        return read() if read is not None else path if path.exists() else None
