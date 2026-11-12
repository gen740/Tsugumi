#!/usr/bin/env python3
"""Foreground Tsugumi execution daemon and its detached job supervisor.

Daemon mode (runs in the foreground; the host registers its endpoint):

    remote_gateway.py --token-file FILE [--host 127.0.0.1] [--port 8765]
                      [--tls-cert PEM --tls-key PEM] [--cgroup-root DIR]
                      [--max-jobs 8]

Transport: authenticated `POST /rpc` only, with `Authorization: Bearer TOKEN`
and exactly one `Content-Length`. The request body is one frame: a 4-byte
big-endian header size (1..65536), a UTF-8 JSON header, then exactly
header["size"] raw bytes for put_blob and submit (no bytes otherwise).
Content-Length must equal the frame length. A 200 response carries a frame of
the same shape, {"version": 1, "ok": ...}; get_blob appends the blob bytes.
Transport failures carry no frame: 400 (bad Content-Length), 401
(authentication), 404 (path), 405 (method), 411 (no Content-Length).

Supervisor mode (internal; spawned only by the daemon with fixed argv and the
job's lock inherited as LOCK_FD):

    remote_gateway.py --supervise PROJECT_ROOT JOB_ID WORKER UNSHARE LOCK_FD CGROUP_ROOT|-
"""

from __future__ import annotations

import argparse
import ctypes
import fcntl
import hashlib
import hmac
import io
import ipaddress
import json
import os
import re
import secrets
import shutil
import signal
import socket
import socketserver
import ssl
import stat
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import BinaryIO, Callable, Optional

import remote_job
from remote_job import (
    CHUNK_BYTES, HEX64, MAX_BLOB_BYTES, MAX_ERROR_CHARS, MAX_OUTPUTS, MAX_SPEC_BYTES,
    ConflictError, Journal, JournalError, _write_atomic, parse_result_manifest,
    stable_identity, validate_result_path, validate_spec,
)

VERSION = 1
# Version of the invocation manifest and worker result exchanged by core.
WORKER_CONTRACT = 1
MAX_HEADER_BYTES = 65536
MAX_REQUEST_BYTES = 4 + MAX_HEADER_BYTES + MAX_BLOB_BYTES
MAX_DIAGNOSTIC = 1024
MAX_RESULT_DEPTH = 64
SUPERVISE = "--supervise"
WORKER_NAME = "tsugumi-remote-worker"
EXECUTOR = "daemon"
ISOLATION = "pid_namespace"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765
DEFAULT_MAX_JOBS = 8
OPERATIONS = {
    "probe": frozenset(),
    "put_blob": frozenset({"sha256", "size"}),
    "get_blob": frozenset({"sha256"}),
    "submit": frozenset({"job_id", "size"}),
    "status": frozenset({"job_id"}),
    "cancel": frozenset({"job_id", "fingerprint"}),
}
OPTIONAL_FIELDS = frozenset({"service", "instance_id", "project_scope", "server_id"})
SERVICE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
TOKEN_RE = re.compile(rb"[0-9a-f]{64}")
LENGTH_RE = re.compile(r"[0-9]{1,20}")
ARCHES = {"x86_64": "x86_64-linux", "aarch64": "aarch64-linux"}
ACTIVE_STATES = frozenset({"queued", "running"})

REQUEST_TIMEOUT_SECONDS = 300
ADMISSION_TIMEOUT_SECONDS = 15
MAX_CONNECTIONS = 64
SCHEDULER_SECONDS = 1.0
POLL_SECONDS = 0.5
# How long the daemon waits for a supervisor to honour a cancellation or its
# deadline before it kills the supervisor (which takes the namespace with it).
KILL_GRACE_SECONDS = 15.0
# Launches of one job that end before any claim before the daemon gives up.
MAX_LAUNCH_ATTEMPTS = 3
CPU_PERIOD_US = 1_000_000
PR_SET_PDEATHSIG = 1
UNSHARE_FLAGS = ("--map-current-user", "--pid", "--fork", "--kill-child=SIGKILL", "--mount-proc")
NO_LIMITS = ("hard cpu_millis and memory_bytes limits need a delegated cgroup v2 subtree; "
             "this execution daemon was started without --cgroup-root, so omit both "
             "limits or restart the daemon with one")


class GatewayError(Exception):
    """Protocol, configuration or process failure that is safe to report."""


class InstanceChanged(GatewayError):
    pass


class RequestRejected(GatewayError):
    pass


class UploadBody:
    """Reads at most `size` bytes from a stream; a short stream is an error."""

    def __init__(self, source: BinaryIO, size: int):
        self.source = source
        self.remaining = size

    def read(self, size: int) -> bytes:
        chunk = self.source.read(min(size, self.remaining))
        if not chunk and self.remaining:
            raise GatewayError("truncated upload")
        self.remaining -= len(chunk)
        return chunk

    def drain(self) -> None:
        while self.remaining:
            self.read(CHUNK_BYTES)


def _diag(text: object, limit: int = MAX_DIAGNOSTIC) -> str:
    return " ".join(str(text).split())[:limit] or "unknown error"


def _describe(exc: BaseException) -> str:
    if isinstance(exc, (GatewayError, JournalError)):
        return str(exc)
    if isinstance(exc, OSError) and exc.strerror:
        return f"{type(exc).__name__}: {exc.strerror}"
    return f"internal error ({type(exc).__name__})"


def _log(message: str) -> None:
    sys.stderr.write(f"tsugumi execution daemon: {_diag(message)}\n")
    sys.stderr.flush()


# ------------------------------------------------------------ environment

def native_system() -> str:
    machine = os.uname().machine
    if not sys.platform.startswith("linux") or machine not in ARCHES:
        raise GatewayError("host is not a supported Linux system")
    return ARCHES[machine]


def state_root() -> Path:
    override = os.environ.get("TSUGUMI_REMOTE_STATE_DIR")
    if override:
        root = Path(override)
    else:
        base = os.environ.get("XDG_STATE_HOME", "")
        base_path = Path(base) if os.path.isabs(base) else Path.home() / ".local" / "state"
        root = base_path / "tsugumi" / "remote-execution"
    if not root.is_absolute():
        raise GatewayError("state directory must be absolute")
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    st = os.lstat(root)
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.geteuid():
        raise GatewayError("state directory is not a private user-owned directory")
    if st.st_mode & 0o077:
        raise GatewayError("state directory must not be accessible by group or others")
    return root


def resolve_worker() -> str:
    name = os.environ.get("TSUGUMI_REMOTE_WORKER") or WORKER_NAME
    path = name if os.sep in name else shutil.which(name)
    if not path:
        raise GatewayError("remote worker executable not found")
    path = os.path.abspath(path)
    if not (os.path.isfile(path) and os.access(path, os.X_OK)):
        raise GatewayError("remote worker is not an executable file")
    return path


def resolve_unshare() -> str:
    path = shutil.which("unshare")
    if not path:
        raise GatewayError("util-linux unshare was not found on PATH")
    return os.path.abspath(path)


def check_namespace(unshare: str) -> None:
    command = [unshare, *UNSHARE_FLAGS, "--", sys.executable, "-c", "import os; assert os.getpid() == 1"]
    try:
        checked = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE, timeout=5, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise GatewayError(f"cannot start an isolated execution namespace: {_describe(exc)}") from None
    if checked.returncode:
        raise GatewayError(f"cannot start an isolated execution namespace: {_diag(checked.stderr.decode(errors='replace'))}")


def load_token(path: str) -> bytes:
    """Read the host-owned bearer token. Errors never include its contents."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError as exc:
        raise GatewayError(f"token file cannot be opened ({exc.strerror})") from None
    with os.fdopen(fd, "rb") as handle:
        st = os.fstat(handle.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid():
            raise GatewayError("token file is not a regular file")
        if st.st_mode & 0o077:
            raise GatewayError("token file must not be accessible by group or others")
        data = handle.read(66)
    if data.endswith(b"\n"):
        data = data[:-1]
    if not TOKEN_RE.fullmatch(data):
        raise GatewayError("token file must contain 64 lowercase hexadecimal characters")
    return data


def check_listen_policy(host: str, tls: bool) -> str:
    """Return the literal address to bind; non-loopback addresses require TLS."""
    literal = "127.0.0.1" if host == "localhost" else host
    try:
        address = ipaddress.ip_address(literal)
    except ValueError:
        raise GatewayError("--host must be a literal IP address or localhost") from None
    if not address.is_loopback and not tls:
        raise GatewayError("listening on a non-loopback address requires --tls-cert and --tls-key")
    return literal


def tls_context(cert: str, key: str) -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(cert, key)
    return context


def acquire_singleton(root: Path) -> int:
    """Hold root/daemon.lock for the daemon's lifetime; one daemon per state root."""
    fd = os.open(root / "daemon.lock", os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        raise GatewayError("another execution daemon is using this state directory") from None
    return fd


# ------------------------------------------------------ process identity

def boot_id() -> str:
    return Path("/proc/sys/kernel/random/boot_id").read_text().strip()


def process_start(pid: int) -> int:
    """Start time of pid in clock ticks since boot (/proc/PID/stat field 22)."""
    raw = Path(f"/proc/{pid}/stat").read_bytes()
    return int(raw[raw.rindex(b")") + 2:].split()[19])


def _read_identity(path: Path) -> Optional[dict]:
    try:
        value = json.loads(path.read_bytes())
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


class ProcessDriver:
    """Starts detached supervisors and signals them only by verified identity."""

    def __init__(self):
        self._children: list = []

    def spawn(self, argv: list, lock_fd: int, log_path: Path) -> None:
        flags = os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW | os.O_CLOEXEC
        log = os.open(log_path, flags, 0o600)
        try:
            # A new session and no parent-death signal: the supervisor outlives
            # the daemon and is adopted again through its job lock.
            child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                     pass_fds=(lock_fd,), start_new_session=True, cwd="/")
        finally:
            os.close(log)
        self._children.append(child)

    def reap(self) -> None:
        self._children = [child for child in self._children if child.poll() is None]

    def kill(self, identity: dict) -> None:
        pid, start = identity.get("pid"), identity.get("start")
        if type(pid) is not int or type(start) is not int or pid <= 1:
            return
        try:
            if identity.get("boot_id") != boot_id():
                return  # recorded before a reboot; that process is gone
            fd = os.pidfd_open(pid)
        except (OSError, AttributeError):
            return
        try:
            # The pidfd pins the process; a matching start time proves it is
            # the recorded supervisor and not a reused PID.
            if process_start(pid) == start:
                signal.pidfd_send_signal(fd, signal.SIGKILL)
        except (OSError, ValueError):
            pass
        finally:
            os.close(fd)


# ---------------------------------------------------------------- cgroups

def _fs_type(path: Path) -> str:
    target = os.path.realpath(path)
    best, kind = "", ""
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        left, _, right = line.partition(" - ")
        fields = left.split()
        if len(fields) < 5 or not right:
            continue
        mount = fields[4].encode().decode("unicode_escape")
        inside = target == mount or target.startswith(mount.rstrip("/") + "/")
        if inside and len(mount) >= len(best):
            best, kind = mount, right.split()[0]
    return kind


class Cgroups:
    """A delegated cgroup v2 subtree; each limited job gets its own child."""

    def __init__(self, root: str):
        path = Path(root)
        if not path.is_absolute():
            raise GatewayError("--cgroup-root must be absolute")
        try:
            st = os.stat(path)
            if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.geteuid():
                raise GatewayError("--cgroup-root must be a directory owned by this account")
            if _fs_type(path) != "cgroup2":
                raise GatewayError("--cgroup-root is not on a cgroup v2 filesystem")
            if not {"cpu", "memory"} <= set((path / "cgroup.controllers").read_text().split()):
                raise GatewayError("--cgroup-root does not delegate the cpu and memory controllers")
            (path / "cgroup.subtree_control").write_text("+cpu +memory")
            if not {"cpu", "memory"} <= set((path / "cgroup.subtree_control").read_text().split()):
                raise GatewayError("--cgroup-root did not enable the cpu and memory controllers")
        except OSError as exc:
            raise GatewayError(f"--cgroup-root is not usable ({exc.strerror or type(exc).__name__})") from None
        self.root = path


def job_cgroup_path(cgroup_root: str, project_root: str, job_id: str) -> Path:
    token = hashlib.sha256(f"{project_root}\0{job_id}".encode()).hexdigest()[:32]
    return Path(cgroup_root) / f"tsugumi-job-{token}"


def remove_job_cgroup(path: Path) -> None:
    if not path.exists():
        return
    try:
        (path / "cgroup.kill").write_text("1")
    except OSError:
        pass
    for _ in range(50):
        try:
            path.rmdir()
            return
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(0.1)  # still populated while the kill completes


def create_job_cgroup(path: Path, cpu_millis: int, memory_bytes: int) -> None:
    remove_job_cgroup(path)  # an empty leftover from an earlier crash
    try:
        path.mkdir(mode=0o700)
        quota = cpu_millis * CPU_PERIOD_US // 1000
        (path / "cpu.max").write_text(f"{quota} {CPU_PERIOD_US}")
        (path / "memory.max").write_text(str(memory_bytes))
        cpu = (path / "cpu.max").read_text().split()
        memory = (path / "memory.max").read_text().strip()
    except OSError as exc:
        raise GatewayError(f"job cgroup could not be configured ({exc.strerror or type(exc).__name__})") from None
    if cpu != [str(quota), str(CPU_PERIOD_US)]:
        raise GatewayError("the kernel did not apply the requested CPU limit")
    page = os.sysconf("SC_PAGE_SIZE")
    if memory != str(memory_bytes // page * page):
        raise GatewayError("the kernel did not apply the requested memory limit")


# ---------------------------------------------------------------- framing

def _read_exact(stream, size: int) -> bytes:
    chunks, remaining = [], size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise GatewayError("request truncated")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_request_header(stream) -> object:
    (size,) = struct.unpack(">I", _read_exact(stream, 4))
    if not 1 <= size <= MAX_HEADER_BYTES:
        raise GatewayError("request header size out of bounds")
    return remote_job._load_json(_read_exact(stream, size), MAX_HEADER_BYTES, "request header")


def encode_frame(header: dict) -> bytes:
    raw = json.dumps(header, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return struct.pack(">I", len(raw)) + raw


def check_header(header: object) -> str:
    if not isinstance(header, dict):
        raise GatewayError("request header must be a JSON object")
    version = header.get("version")
    if type(version) is not int or version != VERSION:
        raise GatewayError("unsupported protocol version")
    op = header.get("operation")
    if not isinstance(op, str) or op not in OPERATIONS:
        raise GatewayError("unsupported operation")
    required = {"version", "operation"} | OPERATIONS[op]
    if not required <= set(header) or set(header) - required - OPTIONAL_FIELDS:
        raise GatewayError("unexpected or missing request fields")
    service = header.get("service", "x")
    if not isinstance(service, str) or not SERVICE_RE.fullmatch(service):
        raise GatewayError("invalid service")
    for key in ("job_id", "sha256", "fingerprint", "instance_id", "project_scope", "server_id"):
        if key in header and not (isinstance(header[key], str) and HEX64.fullmatch(header[key])):
            raise GatewayError(f"invalid {key}")
    if "project_scope" in header:
        if "server_id" not in header:
            raise RequestRejected("scoped requests must carry the pinned server_id")
        if op != "probe" and "instance_id" not in header:
            raise RequestRejected("job requests must carry the pinned project instance_id")
    elif op != "probe":
        raise RequestRejected("a host-authorized project scope is required")
    elif "instance_id" in header:
        raise GatewayError("instance_id requires a project scope")
    if "size" in header:
        high = MAX_SPEC_BYTES if op == "submit" else MAX_BLOB_BYTES
        if type(header["size"]) is not int or not 0 <= header["size"] <= high:
            raise GatewayError("size out of bounds")
    return op


def _payload(state: dict) -> dict:
    status = "cancelling" if state["state"] == "running" and state["cancel_requested"] else state["state"]
    out = {"state": status, "fingerprint": state["fingerprint"],
           "result_sha256": state.get("result_sha256"), "diagnostic": state.get("error")}
    return {k: v for k, v in out.items() if v is not None}


def _fail_unclaimed(journal: Journal, job_id: str, reason: str) -> None:
    claimant = "daemon-" + secrets.token_hex(16)
    if journal.claim(job_id, claimant):  # no-op if someone else claimed
        try:
            journal.finish(job_id, claimant, "failed", error=_diag(reason))
        except ConflictError:
            pass


# ------------------------------------------------------------------ locks

def _open_lock(path: Path) -> int:
    return os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)


def _try_flock(fd: int) -> bool:
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except BlockingIOError:
        return False


# ---------------------------------------------------------------- project

class Project:
    """One host-authorized project scope: its journal and supervisor files."""

    def __init__(self, daemon: "Daemon", scope: str):
        self.daemon = daemon
        self.scope = scope
        self.root = daemon.root / "projects" / scope
        self.journal = Journal(self.root)
        self.run = self.root / "run"
        self.run.mkdir(mode=0o700, exist_ok=True)

    def lock_path(self, job_id: str) -> Path:
        return self.run / f"{job_id}.lock"

    def identity_path(self, job_id: str) -> Path:
        return self.run / f"{job_id}.json"

    def dispatch(self, op: str, header: dict, stream) -> tuple:
        instance = header.get("instance_id", self.journal.instance_id)
        if instance != self.journal.instance_id:
            raise InstanceChanged("execution service points to a different journal")
        if op == "probe":
            return {**self.daemon.capabilities(), "instance_id": self.journal.instance_id}, None
        if op == "put_blob":
            body = UploadBody(stream, header["size"])
            self.journal.put_blob(header["sha256"], header["size"], body)
            body.drain()  # repeated blobs also consume the complete request
            return {"sha256": header["sha256"]}, None
        if op == "get_blob":
            handle = self.journal.open_blob(header["sha256"])
            return {"sha256": header["sha256"], "size": os.fstat(handle.fileno()).st_size}, handle
        job_id = header["job_id"]
        if op == "submit":
            spec = _read_exact(stream, header["size"])
            try:
                self.daemon.check_spec(validate_spec(spec))
                self.journal.submit(job_id, spec)
            except (remote_job.ValidationError, ConflictError) as exc:
                raise RequestRejected(_describe(exc)) from None
            self.daemon.scheduler.notify(self, job_id)
        elif op == "cancel":
            self.journal.cancel(job_id, header["fingerprint"])
            self.daemon.scheduler.notify(self, job_id)
        state = self.journal.status(job_id)
        return ({"state": "missing"} if state is None else _payload(state)), None


# ----------------------------------------------------------------- daemon

class Daemon:
    def __init__(self, root, *, token: bytes, system: str, worker: str, unshare: str,
                 cgroups: Optional[Cgroups] = None, max_jobs: int = DEFAULT_MAX_JOBS,
                 driver=None):
        if type(max_jobs) is not int or max_jobs < 1:
            raise GatewayError("max_jobs must be a positive integer")
        self.root = Path(os.path.abspath(root))
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.server_id = stable_identity(self.root, "server_id")
        self._expected = b"Bearer " + token
        self.system = system
        self.worker = worker
        self.unshare = unshare
        self.cgroups = cgroups
        self.max_jobs = max_jobs
        self._projects: dict = {}
        self._guard = threading.Lock()
        self.scheduler = Scheduler(self, driver or ProcessDriver(), max_jobs)

    def authorized(self, value: Optional[str]) -> bool:
        given = (value or "").encode("latin-1", "replace")
        return hmac.compare_digest(given, self._expected)

    def capabilities(self) -> dict:
        return {"server_id": self.server_id, "native_system": self.system, "systems": [self.system],
                "worker_contract": WORKER_CONTRACT, "executor": EXECUTOR, "isolation": ISOLATION,
                "resource_limits": self.cgroups is not None, "max_jobs": self.max_jobs}

    def check_spec(self, spec: dict) -> None:
        if spec["system"] != self.system:
            raise RequestRejected("spec system is not supported by this host")
        if "cpu_millis" in spec:
            if self.cgroups is None:
                raise RequestRejected(NO_LIMITS)
            if spec["memory_bytes"] < os.sysconf("SC_PAGE_SIZE"):
                raise RequestRejected("requested memory is below the host page size")

    def project(self, scope: str) -> Project:
        with self._guard:
            project = self._projects.get(scope)
            if project is None:
                project = self._projects[scope] = Project(self, scope)
            return project

    def handle(self, header: object, stream) -> tuple:
        op = check_header(header)
        if header.get("server_id", self.server_id) != self.server_id:
            raise InstanceChanged("endpoint now serves a different execution daemon")
        scope = header.get("project_scope")
        if scope is None:
            return self.capabilities(), None  # only probe reaches here
        return self.project(scope).dispatch(op, header, stream)

    def supervisor_argv(self, project: Project, job_id: str, lock_fd: int) -> list:
        return [os.path.abspath(sys.executable), os.path.abspath(__file__), SUPERVISE,
                str(project.root), job_id, self.worker, self.unshare, str(lock_fd),
                str(self.cgroups.root) if self.cgroups else "-"]


class Scheduler:
    """Owns the global queue and cap. A job's supervisor holds its lock file.

    Lock held: a supervisor is alive (launched, adopted or mid-spawn).
    Lock free and queued: pending. Lock free and running: the supervisor ended
    without committing, so the job is interrupted, never run twice.
    """

    def __init__(self, daemon: Daemon, driver, max_jobs: int):
        self.daemon = daemon
        self.driver = driver
        self.max_jobs = max_jobs
        self.wake = threading.Event()
        self.active = 0
        self._lock = threading.Lock()
        self._jobs: dict = {}  # (scope, job_id) -> Project, in FIFO order
        self._stopping_since: dict = {}
        self._launches: dict = {}

    def notify(self, project: Project, job_id: str) -> None:
        with self._lock:
            self._jobs.setdefault((project.scope, job_id), project)
        self.wake.set()

    def recover(self) -> None:
        """Track pending jobs and surviving supervisors after a restart."""
        found = []
        projects = self.daemon.root / "projects"
        for scope_dir in sorted(projects.iterdir()) if projects.is_dir() else ():
            if not (HEX64.fullmatch(scope_dir.name) and scope_dir.is_dir()):
                continue
            project = self.daemon.project(scope_dir.name)
            jobs = {path.stem for path in project.run.glob("*.lock") if HEX64.fullmatch(path.stem)}
            for job_id in project.journal.job_ids():
                state = project.journal.status(job_id)
                if state is not None and state["state"] in ACTIVE_STATES:
                    jobs.add(job_id)
            for job_id in jobs:
                try:
                    order = (project.root / "jobs" / job_id / "spec").stat().st_mtime_ns
                except OSError:
                    order = 0
                found.append((order, project.scope, job_id, project))
        with self._lock:
            for _, scope, job_id, project in sorted(found, key=lambda item: item[:3]):
                self._jobs.setdefault((scope, job_id), project)
        self.wake.set()

    def run(self, stop: threading.Event) -> None:
        while not stop.is_set():
            try:
                self.tick(stop)
            except Exception as exc:
                _log(f"scheduler: {_describe(exc)}")
            self.wake.wait(SCHEDULER_SECONDS)
            self.wake.clear()

    def tick(self, stop: Optional[threading.Event] = None) -> None:
        self.driver.reap()
        with self._lock:
            jobs = list(self._jobs.items())
        active, pending = 0, []
        for key, project in jobs:
            try:
                outcome = self._inspect(project, key)
            except Exception as exc:
                _log(f"job inspection failed: {_describe(exc)}")
                active += 1  # unknown: keep its slot rather than overcommit
                continue
            if outcome == "active":
                active += 1
            elif outcome == "pending":
                pending.append((key, project))
            else:
                with self._lock:
                    self._jobs.pop(key, None)
                self._stopping_since.pop(key, None)
                self._launches.pop(key, None)
        for key, project in pending:
            if active >= self.max_jobs or (stop is not None and stop.is_set()):
                break
            if self._launch(project, key):
                active += 1
        self.active = active

    def _inspect(self, project: Project, key: tuple) -> str:
        job_id = key[1]
        fd = _open_lock(project.lock_path(job_id))
        try:
            if not _try_flock(fd):
                self._enforce(project, key)
                return "active"
            state = project.journal.status(job_id)
            if state is not None and state["state"] == "queued":
                if time.time() < state["deadline"] or not project.journal.expire_queued(job_id):
                    return "pending"
            if state is not None and state["state"] == "running":
                self._lost(project, job_id, state)
            # Terminal and no supervisor: nothing can hold this lock again.
            for path in (project.lock_path(job_id), project.identity_path(job_id)):
                path.unlink(missing_ok=True)
            return "done"
        finally:
            os.close(fd)

    def _enforce(self, project: Project, key: tuple) -> None:
        """Kill a supervisor that ignores its cancellation or deadline."""
        state = project.journal.status(key[1])
        if state is None or state["state"] != "running":
            return
        identity = _read_identity(project.identity_path(key[1]))
        if identity is None:
            return
        now = time.time()
        due = []
        if state["cancel_requested"]:
            due.append(self._stopping_since.setdefault(key, now) + KILL_GRACE_SECONDS)
        if isinstance(identity.get("deadline"), (int, float)):
            due.append(identity["deadline"] + KILL_GRACE_SECONDS)
        if due and now >= min(due):
            self.driver.kill(identity)

    def _lost(self, project: Project, job_id: str, state: dict) -> None:
        error = "execution supervisor ended without recording a result"
        try:
            project.journal.mark_lost(job_id, state["claimant"], error)
        except ConflictError:
            pass  # the supervisor committed first
        discard_work(project.root, job_id)
        if self.daemon.cgroups is not None:
            remove_job_cgroup(job_cgroup_path(str(self.daemon.cgroups.root), str(project.root), job_id))

    def _launch(self, project: Project, key: tuple) -> bool:
        job_id = key[1]
        fd = _open_lock(project.lock_path(job_id))
        try:
            if not _try_flock(fd):
                return True
            state = project.journal.status(job_id)
            if state is None or state["state"] != "queued":
                return False
            attempts = self._launches.get(key, 0)
            if attempts >= MAX_LAUNCH_ATTEMPTS:
                _fail_unclaimed(project.journal, job_id,
                                f"supervisor exited {attempts} times before claiming the job; "
                                f"see {project.run / (job_id + '.log')} on the execution host")
                return False
            self._launches[key] = attempts + 1
            try:
                self.driver.spawn(self.daemon.supervisor_argv(project, job_id, fd), fd,
                                  project.run / f"{job_id}.log")
            except Exception as exc:
                _fail_unclaimed(project.journal, job_id, f"supervisor launch failed: {_describe(exc)}")
                return False
            return True
        finally:
            os.close(fd)  # the supervisor keeps its inherited copy


# --------------------------------------------------------------- transport

def respond(daemon: Daemon, body: UploadBody, http_pin: Optional[str]) -> tuple:
    """Handle one framed request; returns (response frame, blob handle or None, blob size)."""
    handle = None
    try:
        header = read_request_header(body)
        op = check_header(header)
        if header.get("server_id") != http_pin:
            raise InstanceChanged("request frame and HTTP execution daemon identities differ")
        expected = header["size"] if op in ("put_blob", "submit") else 0
        if body.remaining != expected:
            raise GatewayError("Content-Length does not match the request frame")
        response, handle = daemon.handle(header, body)
        size = response["size"] if handle is not None else 0
        return encode_frame({"version": VERSION, "ok": True, **response}), handle, size
    except Exception as exc:
        if handle is not None:
            handle.close()
        response = {"version": VERSION, "ok": False, "error": _diag(_describe(exc))}
        if isinstance(exc, InstanceChanged):
            response["code"] = "instance_changed"
        elif isinstance(exc, RequestRejected):
            response["code"] = "request_rejected"
        return encode_frame(response), None, 0


class RpcHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "tsugumi-execution-daemon"
    sys_version = ""
    timeout = ADMISSION_TIMEOUT_SECONDS

    def log_message(self, *_):
        pass

    def _empty(self, code: int) -> None:
        self.close_connection = True
        self.send_response(code)
        if code == 401:
            self.send_header("WWW-Authenticate", 'Bearer realm="tsugumi"')
        if code == 405:
            self.send_header("Allow", "POST")
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()
        self._linger()

    def _linger(self) -> None:
        """Close gently: unread request bytes would turn the close into a reset
        that can destroy the response before the client reads it."""
        try:
            self.wfile.flush()
            self.connection.shutdown(socket.SHUT_WR)
            self.connection.settimeout(0.5)
            budget = 1 << 16
            while budget > 0:
                chunk = self.connection.recv(min(budget, 8192))
                if not chunk:
                    break
                budget -= len(chunk)
        except OSError:
            pass

    def _not_allowed(self):
        self._empty(405)

    do_GET = do_HEAD = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = _not_allowed

    def _admit(self, *, require_pin: bool = False) -> Optional[tuple]:
        daemon = self.server.execution
        if self.command != "POST":
            return self._empty(405)
        if self.path != "/rpc":
            return self._empty(404)
        auth = self.headers.get_all("Authorization") or []
        if len(auth) != 1 or not daemon.authorized(auth[0]):
            return self._empty(401)
        if self.headers.get("Transfer-Encoding") is not None:
            return self._empty(411)
        lengths = self.headers.get_all("Content-Length") or []
        if not lengths:
            return self._empty(411)
        if len(lengths) != 1 or not LENGTH_RE.fullmatch(lengths[0]) or not 4 <= int(lengths[0]) <= MAX_REQUEST_BYTES:
            return self._empty(400)
        pins = self.headers.get_all("X-Tsugumi-Server") or []
        if len(pins) > 1 or (pins and pins[0] != daemon.server_id) or (require_pin and not pins):
            return self._empty(409)
        return int(lengths[0]), pins[0] if pins else None

    def handle_expect_100(self) -> bool:
        # The host waits for this response before sending any Card bytes.
        # Verify the endpoint identity and authentication before accepting them.
        if self._admit(require_pin=True) is None:
            return False
        return super().handle_expect_100()

    def do_POST(self):
        admitted = self._admit()
        if admitted is None:
            return
        length, pin = admitted
        self.connection.settimeout(REQUEST_TIMEOUT_SECONDS)
        body = UploadBody(self.rfile, length)
        frame, handle, size = respond(self.server.execution, body, pin)
        self.close_connection = True
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(frame) + size))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(frame)
            remaining = size
            while remaining:
                chunk = handle.read(min(CHUNK_BYTES, remaining))
                if not chunk:
                    self.close_connection = True
                    return
                self.wfile.write(chunk)
                remaining -= len(chunk)
            self.wfile.flush()
            if body.remaining:
                self._linger()
        except OSError:
            self.close_connection = True  # the client went away; state is already durable
        finally:
            if handle is not None:
                handle.close()


class RpcServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 64

    def __init__(self, address: tuple, daemon: Daemon, tls: Optional[ssl.SSLContext] = None):
        self.address_family = socket.AF_INET6 if ":" in address[0] else socket.AF_INET
        self.execution = daemon
        self.tls = tls
        self._connections = threading.BoundedSemaphore(MAX_CONNECTIONS)
        super().__init__(address, RpcHandler)

    def server_bind(self):
        # HTTPServer.server_bind does a reverse DNS lookup; skip it.
        socketserver.TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name, self.server_port = host, port

    def get_request(self):
        sock, address = super().get_request()
        sock.settimeout(ADMISSION_TIMEOUT_SECONDS)
        if self.tls is not None:
            # The handshake runs in the connection thread, not the accept loop.
            sock = self.tls.wrap_socket(sock, server_side=True, do_handshake_on_connect=False)
        return sock, address

    def process_request(self, request, address):
        if not self._connections.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self._connections.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self._connections.release()

    def finish_request(self, request, client_address):
        if self.tls is not None:
            try:
                request.do_handshake()
            except OSError:
                return
        super().finish_request(request, client_address)


def _positive(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("must be a positive integer") from None
    if value < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def _port(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("must be a TCP port") from None
    if not 0 <= value <= 65535:
        raise argparse.ArgumentTypeError("must be a TCP port")
    return value


def _interrupt(*_):
    raise KeyboardInterrupt


def serve_main(argv: list) -> int:
    parser = argparse.ArgumentParser(prog="tsugumi-execution-daemon",
                                     description="Run Tsugumi Cards for registered hosts.")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=_port, default=DEFAULT_PORT)
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--tls-cert")
    parser.add_argument("--tls-key")
    parser.add_argument("--cgroup-root")
    parser.add_argument("--max-jobs", type=_positive, default=DEFAULT_MAX_JOBS)
    args = parser.parse_args(argv)
    os.umask(0o077)
    singleton = None
    try:
        if (args.tls_cert is None) != (args.tls_key is None):
            raise GatewayError("--tls-cert and --tls-key must be given together")
        host = check_listen_policy(args.host, args.tls_cert is not None)
        token = load_token(args.token_file)
        tls = tls_context(args.tls_cert, args.tls_key) if args.tls_cert else None
        root = state_root()
        singleton = acquire_singleton(root)
        cgroups = Cgroups(args.cgroup_root) if args.cgroup_root else None
        unshare = resolve_unshare()
        check_namespace(unshare)
        daemon = Daemon(root, token=token, system=native_system(), worker=resolve_worker(),
                        unshare=unshare, cgroups=cgroups, max_jobs=args.max_jobs)
        daemon.scheduler.recover()
        server = RpcServer((host, args.port), daemon, tls)
    except (GatewayError, JournalError, OSError) as exc:
        if singleton is not None:
            os.close(singleton)
        _log(_describe(exc))
        return 1
    stop = threading.Event()
    scheduler = threading.Thread(target=daemon.scheduler.run, args=(stop,), name="scheduler", daemon=True)
    scheduler.start()
    signal.signal(signal.SIGTERM, _interrupt)
    shown = f"[{host}]" if ":" in host else host
    _log(f"listening on {'https' if tls else 'http'}://{shown}:{server.server_address[1]}/rpc "
         f"server_id {daemon.server_id} max_jobs {daemon.max_jobs} "
         f"resource_limits {'on' if cgroups else 'off'}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        # Supervisors keep running; the next daemon adopts them.
        stop.set()
        daemon.scheduler.wake.set()
        server.server_close()
        scheduler.join()
        os.close(singleton)
    return 0


# ------------------------------------------------------------- supervisor

def sandbox_argv(unshare: str, worker: str, job_dir: Path, blob_dir: Path) -> list:
    return [unshare, *UNSHARE_FLAGS, "--", worker, str(job_dir), str(blob_dir)]


def _libc():
    return ctypes.CDLL(None, use_errno=True) if sys.platform.startswith("linux") else None


def _sandbox_child(libc, parent: int, procs: Optional[str]) -> None:
    # Runs between fork and exec of the namespace launcher.
    if libc is not None:
        # The launcher dies with the supervisor, never with the daemon; its
        # --kill-child then tears down the whole PID namespace.
        ulong = ctypes.c_ulong
        status = libc.prctl(ctypes.c_int(PR_SET_PDEATHSIG), ulong(int(signal.SIGKILL)),
                            ulong(0), ulong(0), ulong(0))
        if status != 0 or os.getppid() != parent:
            os._exit(126)
    if procs:
        fd = os.open(procs, os.O_WRONLY)
        try:
            os.write(fd, b"0")  # join the job cgroup before exec
        finally:
            os.close(fd)


def run_sandbox(argv: list, job_dir: Path, should_stop: Callable[[], Optional[str]],
                cgroup: Optional[Path]) -> None:
    """Run the namespace launcher; kill it when should_stop returns a reason."""
    log_path = job_dir / "adapter.log"
    libc, parent = _libc(), os.getpid()
    procs = str(cgroup / "cgroup.procs") if cgroup is not None else None
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    with os.fdopen(os.open(log_path, flags, 0o600), "wb") as log:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                cwd=job_dir, preexec_fn=lambda: _sandbox_child(libc, parent, procs))
        try:
            while True:
                try:
                    rc = proc.wait(timeout=POLL_SECONDS)
                    reason = should_stop()
                    if reason:
                        raise GatewayError(reason)
                    break
                except subprocess.TimeoutExpired:
                    reason = should_stop()
                    if reason:
                        raise GatewayError(reason)
        finally:
            if proc.poll() is None:
                # Still our unreaped child, so the PID cannot have been reused.
                proc.kill()
                proc.wait()
    if rc != 0:
        with open(log_path, "rb") as log:
            log.seek(max(0, os.fstat(log.fileno()).st_size - 2048))
            tail = log.read().decode("utf-8", "replace")
        raise GatewayError(f"worker exited with status {rc}: {tail}")


def _import_file(journal: Journal, dfd: int, name: str, limit: int) -> tuple:
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0)
    with os.fdopen(os.open(name, flags, dir_fd=dfd), "rb") as handle:
        st = os.fstat(handle.fileno())
        if not stat.S_ISREG(st.st_mode) or st.st_size > limit:
            raise GatewayError("result entry changed type during import")
        hasher = hashlib.sha256()
        body = UploadBody(handle, st.st_size)
        while body.remaining:
            chunk = body.read(CHUNK_BYTES)
            hasher.update(chunk)
        handle.seek(0)
        digest = hasher.hexdigest()
        journal.put_blob(digest, st.st_size, handle)  # re-verifies the digest
    return digest, st.st_size


def _walk(journal: Journal, dfd: int, prefix: tuple, out: list, total: int) -> int:
    try:
        if len(prefix) > MAX_RESULT_DEPTH:
            raise GatewayError("result tree too deep")
        names = os.listdir(dfd)
        if len(names) > MAX_OUTPUTS + 1:
            raise GatewayError("too many result entries")
        for name in sorted(names):
            parts = prefix + (name,)
            rel = "/".join(parts)
            validate_result_path(rel)
            st = os.stat(name, dir_fd=dfd, follow_symlinks=False)
            if stat.S_ISDIR(st.st_mode):
                flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
                total = _walk(journal, os.open(name, flags, dir_fd=dfd), parts, out, total)
            elif stat.S_ISREG(st.st_mode):
                if len(out) >= MAX_OUTPUTS + 1:
                    raise GatewayError("too many result files")
                file = _import_file(journal, dfd, name, MAX_BLOB_BYTES - total)
                out.append((rel, file))
                total += file[1]
            else:
                raise GatewayError(f"unsupported result entry {rel!r} (not a regular file)")
        return total
    finally:
        os.close(dfd)


def _import_results(journal: Journal, result_dir: Path) -> str:
    files: list = []
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    _walk(journal, os.open(result_dir, flags), (), files, 0)
    found = dict(files)
    invocation = found.pop("worker.fb", None)
    if invocation is None:
        raise GatewayError("worker did not produce result/worker.fb")
    manifest = json.dumps({
        "invocation_result_sha256": invocation[0],
        "outputs": [{"path": p, "sha256": d, "size": s} for p, (d, s) in sorted(found.items())],
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")
    parse_result_manifest(manifest)
    digest = hashlib.sha256(manifest).hexdigest()
    journal.put_blob(digest, len(manifest), io.BytesIO(manifest))
    return digest


def _execute(journal: Journal, root: Path, job_id: str, run: Callable[[Path, Path], None]) -> str:
    work = root / "work"
    work.mkdir(mode=0o700, exist_ok=True)
    job_dir = work / job_id
    job_dir.mkdir(mode=0o700)  # claim is at most once, so this is always fresh
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    with os.fdopen(os.open(job_dir / "spec.json", flags, 0o600), "wb") as out:
        out.write(journal.spec(job_id))
        out.flush()
        os.fsync(out.fileno())
    run(job_dir, root / "blobs")
    return _import_results(journal, job_dir / "result")


def discard_work(root: Path, job_id: str) -> None:
    work = root / "work" / job_id
    # The worker has stopped. Nix store directories are read-only, and the
    # result tree may contain links. Reopen directories without following
    # links before removing this job's private workspace.
    try:
        for _, _, _, fd in os.fwalk(work, follow_symlinks=False):
            os.fchmod(fd, 0o700)
        shutil.rmtree(work)
    except OSError:
        # A failed cleanup does not change the durable job outcome.
        pass


def _holds_job_lock(fd: int, path: Path) -> bool:
    """True if fd is the inherited, already-locked open file of the job lock."""
    try:
        held, named = os.fstat(fd), os.stat(path, follow_symlinks=False)
        if (held.st_dev, held.st_ino) != (named.st_dev, named.st_ino):
            return False
        if not _try_flock(fd):  # succeeds only on the holder's own open file
            return False
        os.set_inheritable(fd, False)  # the sandbox must not keep the slot
        return True
    except OSError:
        return False


def _write_identity(project: Path, job_id: str, deadline: float) -> None:
    identity = {"pid": os.getpid(), "deadline": deadline}
    try:
        identity.update(start=process_start(os.getpid()), boot_id=boot_id())
    except (OSError, ValueError, IndexError):
        pass  # no /proc: the daemon cannot verify, so it never signals
    _write_atomic(project / "run" / f"{job_id}.json",
                  json.dumps(identity, sort_keys=True).encode("utf-8"))


def supervise(root: str, job_id: str, worker: str, unshare: str, lock_fd: int,
              cgroup_root: str = "-", *, sandbox=run_sandbox) -> int:
    os.umask(0o077)
    if not (HEX64.fullmatch(job_id) and all(os.path.isabs(p) for p in (root, worker, unshare))):
        return 2
    if cgroup_root != "-" and not os.path.isabs(cgroup_root):
        return 2
    project = Path(root)
    if not _holds_job_lock(lock_fd, project / "run" / f"{job_id}.lock"):
        return 2
    journal = Journal(project)
    claimant = "supervisor-" + secrets.token_hex(16)
    if not journal.claim(job_id, claimant):
        return 0  # already claimed, cancelled or finished
    cgroup = None
    try:
        spec = validate_spec(journal.spec(job_id))
        timeout = spec["timeout_seconds"]
        accepted_deadline = journal.status(job_id)["deadline"]
        deadline = time.monotonic() + max(0.0, accepted_deadline - time.time())
        _write_identity(project, job_id, accepted_deadline)

        def should_stop() -> Optional[str]:
            if time.monotonic() >= deadline:
                return f"timed out after {timeout} s"
            try:
                state = journal.status(job_id)
            except (OSError, ValueError):
                return None
            return "cancelled" if state and state["cancel_requested"] else None

        if "cpu_millis" in spec:
            if cgroup_root == "-":
                raise GatewayError(NO_LIMITS)
            cgroup = job_cgroup_path(cgroup_root, root, job_id)
            create_job_cgroup(cgroup, spec["cpu_millis"], spec["memory_bytes"])
        reason = should_stop()
        if reason:
            raise GatewayError(reason)
        result = _execute(journal, project, job_id, lambda job_dir, blobs: sandbox(
            sandbox_argv(unshare, worker, job_dir, blobs), job_dir, should_stop, cgroup))
        reason = should_stop()
        if reason:
            raise GatewayError(reason)
        journal.finish(job_id, claimant, "succeeded", result_sha256=result)
    except Exception as exc:
        # A requested cancellation turns any outcome into "cancelled".
        journal.finish(job_id, claimant, "failed", error=_diag(_describe(exc), MAX_ERROR_CHARS))
    finally:
        if cgroup is not None:
            remove_job_cgroup(cgroup)
        discard_work(project, job_id)
    return 0


def main(argv: Optional[list] = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if args[:1] == [SUPERVISE]:
        if len(args) != 7 or not LENGTH_RE.fullmatch(args[5]):
            return 2
        try:
            return supervise(args[1], args[2], args[3], args[4], int(args[5]), args[6])
        except Exception:
            return 1
    return serve_main(args)


if __name__ == "__main__":
    sys.exit(main())
