# `nix run .#release -- <version>`: cuts a release. When VERSION is not the
# version yet, opens the pull request that sets it; once main carries it, tags
# v<version> and pushes the tag. The release workflow
# (.github/workflows/release.yml) then builds the app and publishes it, and
# installed release builds update themselves to it.
#
#   nix run .#release -- 0.2.0
#   nix run .#release -- 0.2.0-rc.1
#
# Run it from a checkout whose HEAD is origin/main (any branch or worktree).
# The same can be done without a checkout:
#   gh workflow run release.yml -f version=0.2.0
{
  writeShellApplication,
  git,
  gh,
  python3,
}:
writeShellApplication {
  name = "tsugumi-release";
  runtimeInputs = [
    git
    gh
    python3
  ];
  text = ''
    cd "$(git rev-parse --show-toplevel)"
    release="''${1:-}"
    [[ "$release" =~ ^([0-9]+\.[0-9]+\.[0-9]+)(-rc\.([1-9][0-9]*))?$ ]] || { echo "usage: nix run .#release -- <major.minor.patch[-rc.number]>" >&2; exit 2; }
    version="''${BASH_REMATCH[1]}"
    candidate="''${BASH_REMATCH[3]:-}"
    tag="v$release"
    [ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "the working tree has changes" >&2; exit 1; }
    git fetch -q --tags origin main
    [ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || { echo "HEAD is not origin/main" >&2; exit 1; }
    if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
      echo "$tag exists already" >&2
      exit 1
    fi
    last="$(git tag -l 'v*.*.*' | sed -nE 's/^v([0-9]+\.[0-9]+\.[0-9]+)(-rc\.[1-9][0-9]*)?$/\1/p' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)"
    if [ -n "$last" ] && [ "$(printf '%s\n%s\n' "$last" "$version" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)" != "$version" ]; then
      echo "$tag is older than v$last" >&2
      exit 1
    fi
    if [ -n "$candidate" ]; then
      if git rev-parse -q --verify "refs/tags/v$version" >/dev/null; then
        echo "v$version has already been tagged" >&2
        exit 1
      fi
      last_candidate="$(git tag -l "v$version-rc.*" | sed -nE 's/^.*-rc\.([1-9][0-9]*)$/\1/p' | sort -n | tail -1)"
      if [ "$candidate" -le "''${last_candidate:-0}" ]; then
        echo "$tag is older than v$version-rc.$last_candidate" >&2
        exit 1
      fi
    fi
    # main takes changes only through pull requests, so VERSION goes in that way
    # first; the tag is pushed once main carries the version.
    if [ "$(cat VERSION)" != "$version" ]; then
      branch="release/$version"
      git checkout -q -b "$branch"
      python3 -I - "$version" <<'PY'
    import json
    import os
    from pathlib import Path
    import re
    import subprocess
    import sys

    version = sys.argv[1]
    paths = subprocess.check_output(["git", "ls-files", "-z", "*tsugumi.json"])
    changes = {}
    for path in paths.split(b"\0"):
        if not path:
            continue
        manifest = Path(os.fsdecode(path))
        original = manifest.read_bytes()
        expected = json.loads(original)
        if not isinstance(expected.get("tsugumiVersion"), str):
            raise SystemExit(f"{manifest}: expected a tsugumiVersion string")
        expected["tsugumiVersion"] = version
        updated, count = re.subn(
            rb'("tsugumiVersion"\s*:\s*)"[^"]*"',
            lambda match: match[1] + json.dumps(version).encode(),
            original,
        )
        if count != 1 or json.loads(updated) != expected:
            raise SystemExit(f"{manifest}: expected exactly one tsugumiVersion field")
        changes[manifest] = updated

    for manifest, updated in changes.items():
        manifest.write_bytes(updated)
    PY
      echo "$version" > VERSION
      git add -u -- VERSION '*tsugumi.json'
      git commit -qm "Release $version"
      git push -q -u origin "$branch"
      gh pr create --head "$branch" --title "Release $version" \
        --body "Sets \`VERSION\` and the repository's project manifest versions to $version. After this merges, run \`nix run .#release -- $release\` again from origin/main to push the tag \`$tag\`."
      git checkout -q --detach origin/main
      echo "VERSION is not $version yet: merge the pull request above when its checks pass, then run this again." >&2
      exit 0
    fi
    git tag -a "$tag" -m "Tsugumi $release"
    git push origin "refs/tags/$tag"
    echo "$tag pushed: https://github.com/gen740/Tsugumi/actions/workflows/release.yml"
  '';
}
