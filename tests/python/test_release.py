"""What a release build is made of: one version number (VERSION) for every
part, and no path of the build machine (#filePath) outside debug builds."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_every_part_takes_the_version_from_one_file():
    version = (ROOT / "VERSION").read_text().strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+", version)
    assert 'file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TSUGUMI_VERSION' in (ROOT / "CMakeLists.txt").read_text()
    assert "version = lib.fileContents ../VERSION;" in (ROOT / "nix/core.nix").read_text()
    assert "VERSION" in (ROOT / "macOS/Container/vm-source.txt").read_text().split()
    assert "VERSION" in (ROOT / "macOS/build-app.sh").read_text()


def test_the_app_finds_its_resources_on_any_mac():
    # the native build system's Bundle.module looks in the build folder, not in Contents/Resources
    script = (ROOT / "macOS/build-app.sh").read_text()
    assert "build_opts=(-c release --build-system swiftbuild --arch arm64)" in script
    assert 'grep -F "$repo/"' in script


def test_the_dev_build_can_be_signed_with_your_certificate_and_the_release_never_is():
    script = (ROOT / "macOS/build-app.sh").read_text()
    # ad-hoc unless a dev build is given an identity
    assert "\nidentity=-\n" in script
    assert 'if [ "$channel" = dev ] && [ -n "${TSUGUMI_CODESIGN_IDENTITY:-}" ]; then' in script
    assert 'identity="$TSUGUMI_CODESIGN_IDENTITY"' in script
    # the dylibs and the app are signed alike, and nothing is signed ad-hoc by a fixed "-"
    assert 'codesign --force --sign "$identity" "$lib"' in script
    assert 'codesign --force --sign "$identity" --entitlements' in script
    assert "--sign -" not in script


def test_paths_of_the_build_machine_stay_in_debug_builds():
    outside = []
    for path in (ROOT / "macOS/Sources").rglob("*.swift"):
        depth = 0  # how many #if DEBUG blocks the line is in (an #else leaves it)
        stack = []
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            s = line.strip()
            if s.startswith("#if"):
                stack.append(s == "#if DEBUG")
            elif s.startswith("#else") and stack:
                stack[-1] = False
            elif s.startswith("#endif") and stack:
                stack.pop()
            elif "#filePath" in line and not any(stack) and not s.startswith("//"):
                outside.append(f"{path.relative_to(ROOT)}:{n}")
    assert not outside


def test_the_app_and_the_web_ui_keep_api_keys_in_the_same_keychain_item():
    swift = (ROOT / "macOS/Sources/AgentClient/Provider.swift").read_text(encoding="utf-8")
    web = (ROOT / "web/server/chat.ts").read_text(encoding="utf-8")
    service = re.search(r'static let service = "([^"]+)"', swift).group(1)
    assert f"const KEYCHAIN_SERVICE = '{service}'" in web
    # the data protection keychain needs an entitlement ad-hoc builds lack
    assert "kSecUseDataProtectionKeychain as String" not in swift
    for name in ("Anthropic", "OpenAI"):
        assert f"name: '{name}'" in web and f'name: "{name}"' in swift
