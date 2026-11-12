#!/usr/bin/env python3
"""SSH Direct gateway and systemd-supervised launcher for Tsugumi remote jobs.

Gateway mode (no arguments): handle exactly one framed request on binary
stdin/stdout. Frame: 4-byte big-endian header size (1..65536), UTF-8 JSON
header, then a raw body of exactly header["size"] bytes (or none).

Supervisor mode (internal, started only via systemd-run with fixed argv):
    remote_gateway.py --supervise STATE_ROOT JOB_ID WORKER
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import re
import secrets
import shutil
import stat
import struct
import subprocess
import sys
from pathlib import Path
from typing import BinaryIO, Callable, Optional

import remote_job
from remote_job import (
    CHUNK_BYTES, HEX64, MAX_BLOB_BYTES, MAX_ERROR_CHARS, MAX_OUTPUTS, MAX_SPEC_BYTES,
    ConflictError, Journal, JournalError, parse_result_manifest,
    validate_result_path, validate_spec,
)

VERSION = 1
MAX_HEADER_BYTES = 65536
MAX_DIAGNOSTIC = 1024
MAX_RESULT_DEPTH = 64
SUPERVISE = "--supervise"
WORKER_NAME = "tsugumi-remote-worker"
OPERATIONS = {
    "probe": frozenset(),
    "put_blob": frozenset({"sha256", "size"}),
    "get_blob": frozenset({"sha256"}),
    "submit": frozenset({"job_id", "size"}),
    "status": frozenset({"job_id"}),
    "cancel": frozenset({"job_id", "fingerprint"}),
}
SERVICE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
ALIVE = frozenset({"active", "activating", "deactivating", "reloading", "refreshing"})
ARCHES = {"x86_64": "x86_64-linux", "aarch64": "aarch64-linux"}

Runner = Callable[[list], tuple]


class GatewayError(Exception):
    """Protocol, configuration or systemd failure that is safe to report."""


class InstanceChanged(GatewayError):
    pass


class RequestRejected(GatewayError):
    pass


class UploadBody:
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
        os.chmod(root, 0o700)
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


def run_command(argv: list) -> tuple:
    """Run a fixed argv (never a shell); returns (returncode, stdout, stderr)."""
    try:
        proc = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True,
                              timeout=60, check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        raise GatewayError(f"{argv[0]} could not be run ({type(exc).__name__})") from None
    return (proc.returncode, proc.stdout.decode("utf-8", "replace"),
            proc.stderr.decode("utf-8", "replace"))


# ---------------------------------------------------------------- framing

def _read_exact(stream: BinaryIO, size: int) -> bytes:
    chunks, remaining = [], size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise GatewayError("request truncated")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_request_header(stream: BinaryIO) -> object:
    (size,) = struct.unpack(">I", _read_exact(stream, 4))
    if not 1 <= size <= MAX_HEADER_BYTES:
        raise GatewayError("request header size out of bounds")
    return remote_job._load_json(_read_exact(stream, size), MAX_HEADER_BYTES, "request header")


def _write_frame(stream: BinaryIO, header: dict) -> None:
    raw = json.dumps(header, sort_keys=True, separators=(",", ":")).encode("utf-8")
    stream.write(struct.pack(">I", len(raw)) + raw)
    stream.flush()


def check_header(header: object) -> str:
    if not isinstance(header, dict):
        raise GatewayError("request header must be a JSON object")
    version = header.get("version")
    if type(version) is not int or version != VERSION:
        raise GatewayError("unsupported protocol version")
    op = header.get("operation")
    if not isinstance(op, str) or op not in OPERATIONS:
        raise GatewayError("unsupported operation")
    if set(header) - {"service", "instance_id", "project_scope"} != {"version", "operation"} | OPERATIONS[op]:
        raise GatewayError("unexpected or missing request fields")
    service = header.get("service", "x")
    if not isinstance(service, str) or not SERVICE_RE.fullmatch(service):
        raise GatewayError("invalid service")
    for key in ("job_id", "sha256", "fingerprint", "instance_id", "project_scope"):
        if key in header and not (isinstance(header[key], str) and HEX64.fullmatch(header[key])):
            raise GatewayError(f"invalid {key}")
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


# ---------------------------------------------------------------- gateway

class Gateway:
    def __init__(self, root, *, runner: Runner = run_command,
                 worker: Optional[str] = None, system: Optional[str] = None):
        self.root = os.path.abspath(root)
        self.journal = Journal(Path(self.root))
        self.runner = runner
        self._worker = worker
        self._system = system

    @classmethod
    def from_environment(cls, project_scope: str) -> "Gateway":
        return cls(state_root() / "projects" / project_scope)

    def system(self) -> str:
        return self._system or native_system()

    def unit(self, job_id: str) -> str:
        token = f"tsugumi-card\0{self.journal.instance_id}\0{job_id}".encode("ascii")
        return f"tsugumi-card-{hashlib.sha256(token).hexdigest()}.service"

    def dispatch(self, header: object, stream: BinaryIO) -> tuple:
        op = check_header(header)
        if header.get("instance_id", self.journal.instance_id) != self.journal.instance_id:
            raise InstanceChanged("execution service points to a different journal")
        if op == "probe":
            system = self.system()
            return {"instance_id": self.journal.instance_id, "native_system": system,
                    "systems": [system]}, None
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
                validated = validate_spec(spec)
                if validated["system"] != self.system():
                    raise RequestRejected("spec system is not supported by this host")
                if validated["memory_bytes"] < os.sysconf("SC_PAGE_SIZE"):
                    raise RequestRejected("requested memory is below the host page size")
                self.journal.submit(job_id, spec)
            except (remote_job.ValidationError, ConflictError) as exc:
                raise RequestRejected(_describe(exc)) from None
        elif op == "cancel":
            self.journal.cancel(job_id, header["fingerprint"])
        return self.status(job_id), None

    def status(self, job_id: str) -> dict:
        state = self.journal.status(job_id)
        if state is None:
            return {"state": "missing"}
        if state["state"] == "queued":
            self._reconcile_queued(job_id)
        elif state["state"] == "running":
            self._reconcile_running(job_id, state)
        else:
            return _payload(state)
        return _payload(self.journal.status(job_id))

    # -------------------------------------------------------- systemd

    def _query(self, unit: str) -> dict:
        rc, out, _ = self.runner(["systemctl", "--user", "show",
                                  "--property=LoadState,ActiveState,Result", "--", unit])
        props = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
        if (rc != 0 and props.get("LoadState") != "not-found") or "LoadState" not in props or "ActiveState" not in props:
            raise GatewayError("systemd unit query failed")
        return props

    def launch_argv(self, job_id: str) -> list:
        spec = validate_spec(self.journal.spec(job_id))
        worker = self._worker or resolve_worker()
        whole, fraction = divmod(spec["cpu_millis"], 10)
        cpu_percent = f"{whole}.{fraction}" if fraction else str(whole)
        return [
            "systemd-run", "--user", f"--unit={self.unit(job_id)}", "--no-block", "--expand-environment=no",
            "--property=Type=exec",
            f"--property=CPUQuota={cpu_percent}%",
            "--property=CPUQuotaPeriodSec=1s",
            f"--property=MemoryMax={spec['memory_bytes']}",
            f"--property=RuntimeMaxSec={spec['timeout_seconds']}",
            "--property=KillMode=control-group",
            "--property=MemoryAccounting=yes",
            "--property=CPUAccounting=yes",
            "--property=TasksMax=1024",
            "--property=TimeoutStopSec=2s",
            "--", os.path.abspath(sys.executable), os.path.abspath(__file__),
            SUPERVISE, self.root, job_id, worker,
        ]

    def _reconcile_queued(self, job_id: str) -> None:
        unit = self.unit(job_id)
        info = self._query(unit)
        if info["ActiveState"] == "failed":
            reason = ("supervisor unit failed before claiming the job "
                      f"(result {info.get('Result', 'unknown')})")
        elif info["LoadState"] != "not-found":
            return  # starting or running; Journal.claim is at most once
        else:
            try:
                argv = self.launch_argv(job_id)
            except GatewayError as exc:
                reason = f"supervisor launch failed: {exc}"
            else:
                rc, _, err = self.runner(argv)
                if rc == 0:
                    return
                info = self._query(unit)
                if info["LoadState"] != "not-found" and info["ActiveState"] != "failed":
                    return  # concurrent start of the same deterministic unit won
                reason = f"systemd-run failed with status {rc}: {err}"
        self._fail_unclaimed(job_id, reason)

    def _fail_unclaimed(self, job_id: str, reason: str) -> None:
        claimant = "gateway-" + secrets.token_hex(16)
        if self.journal.claim(job_id, claimant):  # no-op if someone else claimed
            try:
                self.journal.finish(job_id, claimant, "failed", error=_diag(reason))
            except ConflictError:
                pass

    def _reconcile_running(self, job_id: str, state: dict) -> None:
        unit = self.unit(job_id)
        info = self._query(unit)
        if info["ActiveState"] in ALIVE:
            if state["cancel_requested"]:
                self._stop(unit)
            return
        error = _diag("supervisor unit ended without recording a result "
                      f"(load {info['LoadState']}, state {info['ActiveState']}, "
                      f"result {info.get('Result', 'unknown')})")
        try:
            self.journal.mark_lost(job_id, state["claimant"], error)
        except ConflictError:
            pass  # supervisor committed its result first
        discard_work(Path(self.root), job_id)

    def _stop(self, unit: str) -> None:
        rc, _, _ = self.runner(["systemctl", "--user", "stop", "--no-block", "--", unit])
        if rc != 0 and self._query(unit)["ActiveState"] in ALIVE:
            raise GatewayError("systemd stop request failed")


def serve(stdin: BinaryIO, stdout: BinaryIO,
          factory: Optional[Callable[[], Gateway]] = None) -> int:
    handle = None
    try:
        header = read_request_header(stdin)
        operation = check_header(header)
        scope = header.get("project_scope")
        if not factory and scope is None:
            if operation != "probe":
                raise RequestRejected("a host-authorized project scope is required")
            system = native_system()
            _write_frame(stdout, {"version": VERSION, "ok": True, "native_system": system, "systems": [system]})
            return 0
        gateway = factory() if factory else Gateway.from_environment(scope)
        response, handle = gateway.dispatch(header, stdin)
    except Exception as exc:
        response = {"version": VERSION, "ok": False, "error": _diag(_describe(exc))}
        if isinstance(exc, InstanceChanged):
            response["code"] = "instance_changed"
        elif isinstance(exc, RequestRejected):
            response["code"] = "request_rejected"
        _write_frame(stdout, response)
        return 1
    try:
        _write_frame(stdout, {"version": VERSION, "ok": True, **response})
        if handle is not None:
            remaining = response["size"]
            while remaining:
                chunk = handle.read(min(CHUNK_BYTES, remaining))
                if not chunk:
                    return 1
                stdout.write(chunk)
                remaining -= len(chunk)
            stdout.flush()
        return 0
    except Exception:
        return 1
    finally:
        if handle is not None:
            handle.close()


# ------------------------------------------------------------- supervisor

def _run_adapter(worker: str, job_dir: Path, blob_dir: Path) -> None:
    log_path = job_dir / "adapter.log"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    with os.fdopen(os.open(log_path, flags, 0o600), "wb") as log:
        rc = subprocess.run([worker, str(job_dir), str(blob_dir)], stdin=subprocess.DEVNULL,
                            stdout=log, stderr=subprocess.STDOUT, cwd=job_dir,
                            check=False).returncode
    if rc != 0:
        with open(log_path, "rb") as log:
            log.seek(max(0, os.fstat(log.fileno()).st_size - 2048))
            tail = log.read().decode("utf-8", "replace")
        raise GatewayError(f"worker adapter exited with status {rc}: {tail}")


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


def _execute(journal: Journal, root: Path, job_id: str, worker: str, adapter) -> str:
    work = root / "work"
    work.mkdir(mode=0o700, exist_ok=True)
    job_dir = work / job_id
    job_dir.mkdir(mode=0o700)  # claim is at most once, so this is always fresh
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    with os.fdopen(os.open(job_dir / "spec.json", flags, 0o600), "wb") as out:
        out.write(journal.spec(job_id))
        out.flush()
        os.fsync(out.fileno())
    adapter(worker, job_dir, root / "blobs")
    return _import_results(journal, job_dir / "result")


def check_limits(spec: dict) -> None:
    groups = Path("/proc/self/cgroup").read_text().splitlines()
    group = next((line.removeprefix("0::/") for line in groups if line.startswith("0::/")), None)
    if not group:
        raise GatewayError("no private cgroup v2 for the execution job")
    cgroup = Path("/sys/fs/cgroup") / group
    quota, period = (cgroup / "cpu.max").read_text().split()
    memory = (cgroup / "memory.max").read_text().strip()
    if quota == "max" or int(quota) * 1000 != int(period) * spec["cpu_millis"]:
        raise GatewayError("systemd did not apply the requested CPU quota")
    page = os.sysconf("SC_PAGE_SIZE")
    expected = spec["memory_bytes"] // page * page
    if memory == "max" or int(memory) != expected or expected <= 0:
        raise GatewayError("systemd did not apply the requested memory limit")


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


def supervise(root: str, job_id: str, worker: str, adapter=None, limit_checker=check_limits) -> int:
    os.umask(0o077)
    if not (HEX64.fullmatch(job_id) and os.path.isabs(root) and os.path.isabs(worker)):
        return 2
    journal = Journal(Path(root))
    claimant = "supervisor-" + secrets.token_hex(16)
    if not journal.claim(job_id, claimant):
        return 0  # already claimed, cancelled or finished
    try:
        limit_checker(validate_spec(journal.spec(job_id)))
        result = _execute(journal, Path(root), job_id, worker, adapter or _run_adapter)
        journal.finish(job_id, claimant, "succeeded", result_sha256=result)
    except Exception as exc:
        journal.finish(job_id, claimant, "failed",
                       error=_diag(_describe(exc), MAX_ERROR_CHARS))
    finally:
        discard_work(Path(root), job_id)
    return 0


def main(argv: Optional[list] = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    try:
        if args and args[0] == SUPERVISE:
            return supervise(*args[1:]) if len(args) == 4 else 2
        if args:
            return 2
        os.umask(0o077)
        return serve(sys.stdin.buffer, sys.stdout.buffer)
    except Exception:
        return 1


if __name__ == "__main__":
    sys.exit(main())
