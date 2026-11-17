"""Enforce native include ownership and independent Swift package boundaries."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
FAMILIES = {
    "util": "util", "rpc": "rpc", "protocol": "protocol",
    "tsugumid/document": "document", "tsugumid/runtime_support": "support",
    "tsugumid": "runtime", "hostd": "host", "workerd/nix": "store", "workerd": "worker",
    "client": "client", "vm/guest": "guest", "sdk": "sdk",
}
ALLOWED = {
    "util": {"util"}, "rpc": {"rpc", "util"}, "protocol": {"protocol"},
    "document": {"document", "util", "protocol"},
    "store": {"store"},
    "support": {"support", "store", "util", "protocol"},
    "runtime": {"runtime", "support", "document", "worker", "store", "util", "rpc", "protocol"},
    "host": {"host", "document", "worker", "util", "rpc", "protocol", "client", "sdk"},
    "worker": {"worker", "store", "document", "util", "rpc", "protocol", "sdk"},
    "client": {"client", "rpc", "protocol"}, "sdk": {"sdk", "util", "protocol"},
    "guest": {"guest", "support", "store", "util", "protocol"},
}
INCLUDES = [ROOT / path for path in (
    "util/include", "rpc/include", "rpc/tls/include", "tsugumid/document/include",
    "tsugumid/runtime_support/include", "tsugumid/include", "workerd/include",
    "workerd/nix/include", "client/include", "sdk/include", "sdk/describe",
    "hostd/private", "hostd/agent/private", "tsugumid/runtime/src", "workerd/src",
)]


def owner(path):
    path = path.resolve()
    for prefix, name in FAMILIES.items():
        if path.is_relative_to(ROOT / prefix):
            return name
    return None


def can_include(source_owner, path):
    dependency = owner(path)
    if source_owner == "host" and dependency == "worker":
        return path.resolve() in {ROOT / "workerd/include/tsugumi/workerd/protocol.hh", ROOT / "workerd/include/tsugumi/workerd/scratch_policy.hh"}
    return dependency is None or dependency in ALLOWED[source_owner]


def check_swift():
    failures = []
    packages = {
        "client/swift": (set(), {"https://github.com/google/flatbuffers.git"}),
        "vm/macos/runtime": ({"../../../client/swift"}, {"https://github.com/google/flatbuffers.git"}),
    }
    for prefix, (local, remote) in packages.items():
        manifest = ROOT / prefix / "Package.swift"
        text = manifest.read_text()
        for key, allowed in (("path", local), ("url", remote)):
            for dependency in re.findall(r'\.package\(\s*(?:name:\s*"[^"]+",\s*)?' + key + r':\s*"([^"]+)"', text):
                if dependency not in allowed:
                    failures.append(f"{manifest.relative_to(ROOT)}: forbidden {key} dependency {dependency}")
    modules = {
        "client/swift/Sources/RPC": {"Foundation", "CryptoKit", "FlatBuffers", "TsugumiClientC"},
        "vm/macos/runtime/Sources/VMTypes": {"Foundation"},
        "vm/macos/runtime/Sources/VMRuntime": {"Foundation", "Darwin", "CryptoKit", "Virtualization", "TsugumiVMTypes"},
        "vm/macos/runtime/Sources/VMHost": {"Foundation", "Darwin", "Virtualization", "FlatBuffers", "TsugumiRPC", "TsugumiVMTypes", "TsugumiVMRuntime"},
    }
    for prefix, allowed in modules.items():
        sources = sorted((ROOT / prefix).rglob("*.swift"))
        if not sources:
            failures.append(f"Missing Swift responsibility sources: {prefix}")
        for path in sources:
            for number, line in enumerate(path.read_text().splitlines(), 1):
                match = re.match(r'\s*(?:@\w+(?:\([^)]*\))?\s+)*import\s+(?:(?:struct|class|enum|protocol|typealias|func|var|let)\s+)?(\w+)', line)
                if match and match[1] not in allowed:
                    failures.append(f"{path.relative_to(ROOT)}:{number}: forbidden Swift import {match[1]}")
    for path in (ROOT / "ui/macos/Sources").rglob("*.swift"):
        if re.search(r'\bimport\s+TsugumiVMRuntime\b', path.read_text()):
            failures.append(f"{path.relative_to(ROOT)}: UI cannot import VM mechanics")
    return failures


def check():
    failures = []
    seen = set()
    for prefix in FAMILIES:
        for path in sorted((ROOT / prefix).rglob("*")):
            if path.suffix not in {".cc", ".hh"} or path in seen:
                continue
            seen.add(path)
            source_owner = owner(path)
            for number, line in enumerate(path.read_text().splitlines(), 1):
                match = re.match(r'\s*#\s*include\s*["<]([^">]+)[">]', line)
                if not match:
                    continue
                include = match[1]
                if include.startswith("tsugumi/protocol/"):
                    destination = ROOT / "protocol" / include
                else:
                    candidates = [path.parent / include, *(base / include for base in INCLUDES), ROOT / include]
                    destination = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
                if destination and not can_include(source_owner, destination):
                    failures.append(f"{path.relative_to(ROOT)}:{number}: {source_owner} cannot include {destination.relative_to(ROOT)}")
    for prefix in FAMILIES:
        if not (ROOT / prefix).is_dir():
            failures.append(f"Missing responsibility root: {prefix}")
    return failures + check_swift()


if __name__ == "__main__":
    failures = check()
    if failures:
        print("\n".join(failures), file=sys.stderr)
        sys.exit(1)
