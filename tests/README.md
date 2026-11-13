# Repository tests

Run commands from the repository root. Build instructions are in [Installing and building](../docs/INSTALL.md#building-from-source); [core/README.md](core/README.md) gives the core suite's detailed workflow.

Run `tests/check-fast.sh` before every push: the static tests (`tests/static`: tests that only read files of the repository, which CI runs first in its own job) and the Nix checks of the Web UI and the site player (`--no-nix` skips these). Run the platform checks with `tests/ci-local.sh`. On macOS this runs the Mac checks locally and the Linux checks in Docker or OrbStack. Use `tests/ci-local.sh mac` or `tests/ci-local.sh linux` for one platform, and `tests/ci-local.sh agent` for the public Agent RPC fixtures.

The CI definitions are [ci.yml](../.github/workflows/ci.yml) and [ci-platform.yml](../.github/workflows/ci-platform.yml).

| Location | Coverage |
| --- | --- |
| [tests/core/](core/README.md) | Core behavior, public RPC and integration tests |
| `tests/python/` | Packaging, release, licenses and host integration |
| `tests/static/` | Repository contracts and generated-file consistency |
| `tests/remote/` | C++ remote worker admission, scoping, transfers and recovery on Linux |
| `macOS/CoreTests/`, `macOS/Tests/` | Agent, RPC and app behavior |
| `web/` | Browser UI type checks and behavior contracts |
| `tests/prompt/` | Offline prompt probe scoring |

The build generates C++/Python/Swift/TypeScript bindings and BFBS with `flatc`; these stay in build outputs and are not committed.

For documentation changes, run `nix run .#build-doc` and `nix run .#test-doc`. Changes to the demo player also require `node docs/site/player/build.mjs --check`.

## Manual tools

- `nix develop -c /usr/bin/swift tests/tools/wkwebgpu.swift`: probe WebGPU and adapter availability in an offscreen WKWebView.
- `python3 sdk/codegen/scaffold_go.py`: synchronize the Swift and TypeScript Go scaffolds after editing `sdk/go/template/` (`--check` verifies them).
- `python3 sdk/codegen/scaffold_rust.py`: synchronize the Swift and TypeScript Rust scaffolds after editing `sdk/rust/template/` (`--check` verifies them).
