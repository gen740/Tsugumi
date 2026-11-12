"""Durable job journal and content-addressed blob store for Tsugumi remote jobs.

The journal lives on a private POSIX storage root that only the execution
daemon and its job supervisors use. Layout:

    root/instance_id          stable random 64-hex identity of this journal
    root/blobs/<sha256>       immutable, verified content
    root/jobs/<job_id>/spec   exact submitted spec bytes (immutable)
    root/jobs/<job_id>/state.json   current state, replaced atomically
    root/locks/<job_id>       per-job flock file
    root/tmp/                 staging area for incomplete blobs

Job directories are never deleted, so job identities are never reused.
"""

from __future__ import annotations

import contextlib
import fcntl
import hashlib
import json
import os
import re
import secrets
import struct
import tempfile
import time
from pathlib import Path
from typing import BinaryIO, Iterator, Optional

HEX64 = re.compile(r"[0-9a-f]{64}")
CLAIMANT_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}")
STORE_PATH_RE = re.compile(
    r"/nix/store/[0-9abcdfghijklmnpqrsvwxyz]{32}-[A-Za-z0-9+_?=-][A-Za-z0-9+._?=-]{0,210}"
)

CHUNK_BYTES = 1 << 20
MAX_BLOB_BYTES = 64 << 30
MAX_SPEC_BYTES = 256 << 10
MAX_INPUT_BLOBS = 1024
MAX_CPU_MILLIS = 256_000
MAX_MEMORY_BYTES = 1 << 40
MAX_TIMEOUT_SECONDS = 7 * 24 * 3600
MAX_MANIFEST_BYTES = 8 << 20
MAX_OUTPUTS = 65536
MAX_PATH_BYTES = 4096
MAX_COMPONENT_BYTES = 255
MAX_ENVELOPE_BYTES = 16 << 20
MAX_ERROR_CHARS = 4096

SYSTEMS = frozenset({"aarch64-linux", "x86_64-linux"})
SPEC_KEYS = frozenset({
    "system", "derivation", "output", "invocation_sha256", "input_blobs",
    "timeout_seconds",
})
# Hard resource limits are optional, but only as a pair.
LIMIT_KEYS = frozenset({"cpu_millis", "memory_bytes"})
MANIFEST_KEYS = frozenset({"outputs", "invocation_result_sha256"})
OUTPUT_KEYS = frozenset({"path", "sha256", "size"})
FINISH_STATES = frozenset({"succeeded", "failed", "interrupted"})


class JournalError(Exception):
    """Base class for journal errors."""


class ValidationError(JournalError, ValueError):
    """Input is malformed, out of bounds, or references missing content."""


class ConflictError(JournalError):
    """Request conflicts with already committed state."""


class NotFoundError(JournalError, LookupError):
    """Requested job, spec, or blob does not exist."""


# ---------------------------------------------------------------- helpers

def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _check_digest(value: object, what: str = "sha256 digest") -> str:
    if not isinstance(value, str) or not HEX64.fullmatch(value):
        raise ValidationError(f"invalid {what}")
    return value


def _check_int(value: object, low: int, high: int, what: str) -> int:
    if type(value) is not int or not low <= value <= high:  # rejects bool
        raise ValidationError(f"{what} must be an integer in [{low}, {high}]")
    return value


def _check_claimant(value: object) -> str:
    if not isinstance(value, str) or not CLAIMANT_RE.fullmatch(value):
        raise ValidationError("invalid claimant")
    return value


def _check_error(value: object, required: bool) -> Optional[str]:
    if value is None and not required:
        return None
    if not isinstance(value, str) or not 1 <= len(value) <= MAX_ERROR_CHARS:
        raise ValidationError("error must be a non-empty bounded string")
    return value


def _fsync_dir(path: Path) -> None:
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _write_atomic(path: Path, data: bytes) -> None:
    """Write data to a temp file, fsync it, rename over path, fsync the dir."""
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, path)
    except BaseException:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(tmp)
        raise
    _fsync_dir(path.parent)


def _load_json(data: bytes, limit: int, what: str) -> object:
    """Strict bounded UTF-8 JSON: no duplicate keys, NaN or Infinity."""
    if len(data) > limit:
        raise ValidationError(f"{what} exceeds {limit} bytes")

    def pairs(items):
        obj = {}
        for key, value in items:
            if key in obj:
                raise ValidationError(f"duplicate key {key!r} in {what}")
            obj[key] = value
        return obj

    def bad_constant(name):
        raise ValidationError(f"{name} is not allowed in {what}")

    try:
        text = data.decode("utf-8")
        return json.loads(text, object_pairs_hook=pairs, parse_constant=bad_constant)
    except ValidationError:
        raise
    except (ValueError, RecursionError) as exc:  # includes decode errors
        raise ValidationError(f"{what} is not valid UTF-8 JSON") from exc


# ------------------------------------------------------------- validation

def validate_spec(data: bytes) -> dict:
    """Parse and validate job spec bytes; returns the parsed object."""
    spec = _load_json(data, MAX_SPEC_BYTES, "spec")
    if not isinstance(spec, dict) or set(spec) not in (SPEC_KEYS, SPEC_KEYS | LIMIT_KEYS):
        raise ValidationError("spec must be an object with exactly the keys "
                              + ", ".join(sorted(SPEC_KEYS))
                              + ", plus both or neither of cpu_millis and memory_bytes")
    if not isinstance(spec["system"], str) or spec["system"] not in SYSTEMS:
        raise ValidationError("unsupported system")
    for key in ("derivation", "output"):
        if not isinstance(spec[key], str) or not STORE_PATH_RE.fullmatch(spec[key]):
            raise ValidationError(f"{key} must be a single canonical /nix/store path")
    if not spec["derivation"].endswith(".drv"):
        raise ValidationError("derivation must end with .drv")
    invocation = _check_digest(spec["invocation_sha256"], "invocation_sha256")
    blobs = spec["input_blobs"]
    if not isinstance(blobs, list) or not 1 <= len(blobs) <= MAX_INPUT_BLOBS:
        raise ValidationError("input_blobs must be a non-empty bounded array")
    for digest in blobs:
        _check_digest(digest, "input blob digest")
    if len(set(blobs)) != len(blobs):
        raise ValidationError("input_blobs contains duplicates")
    if invocation not in blobs:
        raise ValidationError("input_blobs must include invocation_sha256")
    if "cpu_millis" in spec:
        _check_int(spec["cpu_millis"], 1, MAX_CPU_MILLIS, "cpu_millis")
        _check_int(spec["memory_bytes"], 1, MAX_MEMORY_BYTES, "memory_bytes")
    _check_int(spec["timeout_seconds"], 1, MAX_TIMEOUT_SECONDS, "timeout_seconds")
    return spec


def validate_result_path(path: object) -> tuple:
    """Validate a safe POSIX relative output path; returns its components."""
    if not isinstance(path, str) or not path:
        raise ValidationError("output path must be a non-empty string")
    if "\\" in path or any(ord(c) < 0x20 or 0x7F <= ord(c) <= 0x9F for c in path):
        raise ValidationError("output path contains a backslash or control character")
    try:
        encoded = path.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValidationError("output path is not valid UTF-8") from exc
    if len(encoded) > MAX_PATH_BYTES or path.startswith("/"):
        raise ValidationError("output path is absolute or too long")
    parts = tuple(path.split("/"))
    for part in parts:
        if part in ("", ".", "..") or len(part.encode("utf-8")) > MAX_COMPONENT_BYTES:
            raise ValidationError(f"unsafe output path {path!r}")
    return parts


def parse_result_manifest(data: bytes) -> dict:
    """Validate result manifest bytes (schema only, not blob presence).

    Schema: {"outputs": [{"path": str, "sha256": hex64, "size": int}, ...],
             "invocation_result_sha256": hex64}
    """
    manifest = _load_json(data, MAX_MANIFEST_BYTES, "result manifest")
    if not isinstance(manifest, dict) or set(manifest) != MANIFEST_KEYS:
        raise ValidationError("result manifest must have exactly outputs, invocation_result_sha256")
    _check_digest(manifest["invocation_result_sha256"], "invocation_result_sha256")
    outputs = manifest["outputs"]
    if not isinstance(outputs, list) or not 1 <= len(outputs) <= MAX_OUTPUTS:
        raise ValidationError("outputs must be a non-empty bounded array")
    paths = {}
    for entry in outputs:
        if not isinstance(entry, dict) or set(entry) != OUTPUT_KEYS:
            raise ValidationError("output entries must have exactly path, sha256, size")
        parts = validate_result_path(entry["path"])
        _check_digest(entry["sha256"], "output sha256")
        _check_int(entry["size"], 0, MAX_BLOB_BYTES, "output size")
        if entry["path"] in paths:
            raise ValidationError(f"duplicate output path {entry['path']!r}")
        paths[entry["path"]] = parts
    for path, parts in paths.items():
        for i in range(1, len(parts)):
            if "/".join(parts[:i]) in paths:
                raise ValidationError(f"output path {path!r} conflicts with a file prefix")
    return manifest


def check_flatbuffer_frame(buf: bytes) -> None:
    """Structural bounds check of a FlatBuffers root table (not schema-level)."""
    size = len(buf)
    if not 12 <= size <= MAX_ENVELOPE_BYTES:
        raise ValidationError("result envelope size out of bounds")
    (root,) = struct.unpack_from("<I", buf, 0)
    if root % 4 or not 4 <= root <= size - 4:
        raise ValidationError("result envelope root offset out of bounds")
    (soffset,) = struct.unpack_from("<i", buf, root)
    vtable = root - soffset
    if vtable < 4 or vtable % 2 or vtable + 4 > size:
        raise ValidationError("result envelope vtable out of bounds")
    vtable_len, table_len = struct.unpack_from("<HH", buf, vtable)
    if (vtable_len < 4 or vtable_len % 2 or vtable + vtable_len > size
            or table_len < 4 or root + table_len > size):
        raise ValidationError("result envelope table out of bounds")


def stable_identity(directory: Path, name: str) -> str:
    """Return directory/name, a random 64-hex identity created once and never replaced."""
    path = Path(directory) / name
    if not path.exists():
        fd, tmp = tempfile.mkstemp(dir=directory, prefix=".tmp-id-")
        try:
            with os.fdopen(fd, "wb") as out:
                out.write(secrets.token_hex(32).encode("ascii") + b"\n")
                out.flush()
                os.fsync(out.fileno())
            with contextlib.suppress(FileExistsError):
                os.link(tmp, path)  # first creator wins, never overwritten
        finally:
            os.unlink(tmp)
        _fsync_dir(Path(directory))
    value = path.read_bytes().strip().decode("ascii", "replace")
    if not HEX64.fullmatch(value):
        raise JournalError(f"corrupt {name}")
    return value


def _new_state(fingerprint: str, state: str) -> dict:
    return {
        "state": state,
        "fingerprint": fingerprint,
        "claimant": None,
        "result_sha256": None,
        "error": None,
        "deadline": None,
        # Internal: cancellation requested while running.
        "cancel_requested": False,
        # Internal: digest of the committed terminal request, for idempotency.
        # A digest (not the request) so a cancelled job never exposes a result.
        "outcome_sha256": None,
    }


# ---------------------------------------------------------------- journal

class Journal:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)
        self._blobs = self.root / "blobs"
        self._jobs = self.root / "jobs"
        self._locks = self.root / "locks"
        self._tmp = self.root / "tmp"
        for directory in (self._blobs, self._jobs, self._locks, self._tmp):
            directory.mkdir(mode=0o700, exist_ok=True)
        _fsync_dir(self.root)
        self.instance_id = stable_identity(self.root, "instance_id")

    def job_ids(self) -> list:
        return sorted(name for name in os.listdir(self._jobs) if HEX64.fullmatch(name))

    # ------------------------------------------------------------ blobs

    def _blob_path(self, digest: str) -> Path:
        return self._blobs / _check_digest(digest)

    def _blob_size(self, digest: str) -> Optional[int]:
        try:
            return self._blob_path(digest).stat().st_size
        except FileNotFoundError:
            return None

    def put_blob(self, digest: str, size: int, stream: BinaryIO) -> dict:
        """Read exactly `size` bytes from stream, verify, publish immutably."""
        path = self._blob_path(digest)
        _check_int(size, 0, MAX_BLOB_BYTES, "blob size")
        existing = self._blob_size(digest)
        if existing is not None:
            if existing != size:
                raise ConflictError("blob already exists with a different size")
            return {"sha256": digest, "size": size}
        hasher = hashlib.sha256()
        fd, tmp = tempfile.mkstemp(dir=self._tmp, prefix="blob-")
        try:
            with os.fdopen(fd, "wb") as out:
                remaining = size
                while remaining:
                    chunk = stream.read(min(CHUNK_BYTES, remaining))
                    if not chunk:
                        raise ValidationError("stream ended before declared size")
                    if len(chunk) > remaining:
                        raise ValidationError("stream returned more than requested")
                    hasher.update(chunk)
                    out.write(chunk)
                    remaining -= len(chunk)
                out.flush()
                os.fsync(out.fileno())
            if hasher.hexdigest() != digest:
                raise ValidationError("blob content does not match digest")
            os.chmod(tmp, 0o444)
            with contextlib.suppress(FileExistsError):
                os.link(tmp, path)  # atomic, never replaces published content
            _fsync_dir(self._blobs)
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(tmp)
        return {"sha256": digest, "size": size}

    def open_blob(self, digest: str) -> BinaryIO:
        try:
            return open(self._blob_path(digest), "rb")
        except FileNotFoundError:
            raise NotFoundError(f"blob {digest} not found") from None

    def _read_blob_bounded(self, digest: str, limit: int, what: str) -> bytes:
        size = self._blob_size(digest)
        if size is None:
            raise ValidationError(f"{what} blob {digest} is not published")
        if size > limit:
            raise ValidationError(f"{what} exceeds {limit} bytes")
        with self.open_blob(digest) as handle:
            data = handle.read(limit + 1)
        if _sha256(data) != digest:
            raise JournalError(f"{what} blob {digest} is corrupt on disk")
        return data

    # ------------------------------------------------------------- jobs

    @contextlib.contextmanager
    def _locked(self, job_id: str) -> Iterator[Path]:
        _check_digest(job_id, "job id")
        flags = os.O_RDWR | os.O_CREAT | getattr(os, "O_CLOEXEC", 0)
        fd = os.open(self._locks / job_id, flags, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            yield self._jobs / job_id
        finally:
            os.close(fd)  # releases the flock

    @staticmethod
    def _read_state(job_dir: Path) -> Optional[dict]:
        try:
            return json.loads((job_dir / "state.json").read_bytes())
        except FileNotFoundError:
            return None

    def _write_state(self, job_dir: Path, state: dict) -> None:
        self._ensure_dir(job_dir)
        _write_atomic(job_dir / "state.json",
                      json.dumps(state, sort_keys=True).encode("utf-8"))

    def _ensure_dir(self, job_dir: Path) -> None:
        try:
            job_dir.mkdir(mode=0o700)
        except FileExistsError:
            return
        _fsync_dir(self._jobs)

    def status(self, job_id: str) -> Optional[dict]:
        return self._read_state(self._jobs / _check_digest(job_id, "job id"))

    def spec(self, job_id: str) -> bytes:
        job_dir = self._jobs / _check_digest(job_id, "job id")
        state = self._read_state(job_dir)
        if state is None:
            raise NotFoundError(f"job {job_id} not found")
        try:
            data = (job_dir / "spec").read_bytes()
        except FileNotFoundError:
            raise NotFoundError(f"job {job_id} has no accepted spec") from None
        if _sha256(data) != state["fingerprint"]:
            # Leftover from an interrupted submit before a cancel tombstone.
            raise NotFoundError(f"job {job_id} has no accepted spec")
        return data

    def submit(self, job_id: str, spec: bytes) -> dict:
        _check_digest(job_id, "job id")
        if not isinstance(spec, (bytes, bytearray)):
            raise ValidationError("spec must be bytes")
        spec = bytes(spec)
        if len(spec) > MAX_SPEC_BYTES:
            raise ValidationError(f"spec exceeds {MAX_SPEC_BYTES} bytes")
        fingerprint = _sha256(spec)
        with self._locked(job_id) as job_dir:
            current = self._read_state(job_dir)
            if current is not None:
                if current["fingerprint"] != fingerprint:
                    raise ConflictError("job id already used with a different spec")
                return current
            parsed = validate_spec(spec)
            missing = [d for d in parsed["input_blobs"] if self._blob_size(d) is None]
            if missing:
                raise ValidationError(f"input blobs not published: {', '.join(missing)}")
            self._ensure_dir(job_dir)
            _write_atomic(job_dir / "spec", spec)  # spec before state
            state = _new_state(fingerprint, "queued")
            state["deadline"] = time.time() + parsed["timeout_seconds"]
            self._write_state(job_dir, state)
            return state

    def _expire_queued(self, job_dir: Path, state: Optional[dict]) -> bool:
        if state is None or state["state"] != "queued" or time.time() < state["deadline"]:
            return False
        state.update(state="failed", error="timed out after waiting for an execution slot")
        self._write_state(job_dir, state)
        return True

    def expire_queued(self, job_id: str) -> bool:
        with self._locked(job_id) as job_dir:
            return self._expire_queued(job_dir, self._read_state(job_dir))

    def claim(self, job_id: str, claimant: str) -> bool:
        _check_claimant(claimant)
        with self._locked(job_id) as job_dir:
            state = self._read_state(job_dir)
            if state is None or state["state"] != "queued":
                return False
            if self._expire_queued(job_dir, state):
                return False
            state.update(state="running", claimant=claimant)
            self._write_state(job_dir, state)
            return True

    def cancel(self, job_id: str, fingerprint: str) -> dict:
        _check_digest(fingerprint, "fingerprint")
        with self._locked(job_id) as job_dir:
            state = self._read_state(job_dir)
            if state is None:
                state = _new_state(fingerprint, "cancelled")  # tombstone
            elif state["fingerprint"] != fingerprint:
                raise ConflictError("fingerprint does not match job")
            elif state["state"] == "queued":
                state["state"] = "cancelled"
            elif state["state"] == "running" and not state["cancel_requested"]:
                state["cancel_requested"] = True
            else:
                return state  # terminal, or already requested
            self._write_state(job_dir, state)
            return state

    def finish(self, job_id: str, claimant: str, state: str,
               result_sha256: Optional[str] = None,
               error: Optional[str] = None) -> dict:
        _check_claimant(claimant)
        if not isinstance(state, str) or state not in FINISH_STATES:
            raise ValidationError("finish state must be succeeded, failed or interrupted")
        if state == "succeeded":
            _check_digest(result_sha256, "result_sha256")
            if error is not None:
                raise ValidationError("succeeded jobs carry no error")
        else:
            if result_sha256 is not None:
                raise ValidationError("only succeeded jobs carry a result")
            _check_error(error, required=False)
        request = {"op": "finish", "state": state,
                   "result_sha256": result_sha256, "error": error}
        return self._terminate(job_id, claimant, request)

    def mark_lost(self, job_id: str, claimant: str, error: str) -> dict:
        """Record that the driver verified the claimed supervisor ended."""
        _check_claimant(claimant)
        _check_error(error, required=True)
        return self._terminate(job_id, claimant, {"op": "lost", "error": error})

    def _terminate(self, job_id: str, claimant: str, request: dict) -> dict:
        outcome = _sha256(json.dumps(request, sort_keys=True).encode("utf-8"))
        with self._locked(job_id) as job_dir:
            state = self._read_state(job_dir)
            if state is None:
                raise NotFoundError(f"job {job_id} not found")
            if state["claimant"] != claimant:
                raise ConflictError("caller does not own the claim")
            if state["state"] != "running":
                if state["outcome_sha256"] == outcome:
                    return state  # exact duplicate of the committed request
                raise ConflictError("job already finished with a different outcome")
            if state["cancel_requested"]:
                final, result, error = "cancelled", None, None
            elif request["op"] == "lost":
                final, result, error = "interrupted", None, request["error"]
            else:
                final, result, error = request["state"], request["result_sha256"], request["error"]
                if final == "succeeded":
                    self._verify_result(result)
            state.update(state=final, result_sha256=result, error=error,
                         outcome_sha256=outcome)
            self._write_state(job_dir, state)
            return state

    def _verify_result(self, digest: str) -> None:
        manifest = parse_result_manifest(
            self._read_blob_bounded(digest, MAX_MANIFEST_BYTES, "result manifest"))
        check_flatbuffer_frame(self._read_blob_bounded(
            manifest["invocation_result_sha256"], MAX_ENVELOPE_BYTES, "result envelope"))
        for entry in manifest["outputs"]:
            if self._blob_size(entry["sha256"]) != entry["size"]:
                raise ValidationError(f"output {entry['path']!r} missing or wrong size")
