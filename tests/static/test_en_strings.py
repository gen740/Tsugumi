"""The English UI strings (macOS/Resources/tools/en_strings.py)."""
import importlib.util
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "macOS/Resources/tools/en_strings.py"


def load():
    spec = importlib.util.spec_from_file_location("en_strings", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_every_translation_takes_the_arguments_of_its_source():
    # A translation that reorders %@ and %llu without positions (%2$llu)
    # would read an integer as an object and crash the app.
    en = load()
    wrong = {k: v for k, v in en.EN.items() if en.specifiers(k) != en.specifiers(v)}
    assert not wrong


def test_positions_are_read_in_order():
    en = load()
    assert en.specifiers("Run %@ の %llu 回目") == ["@", "llu"]
    assert en.specifiers("invocation %2$llu in Run %1$@") == ["@", "llu"]
    assert en.specifiers("100%% %lld") == ["lld"]


def test_the_bundled_strings_are_the_generated_ones():
    # Exactly the generated text: build-app.sh regenerates the file from EN, so a string
    # added only to Localizable.strings (not EN) would be lost, untranslated, in the built app.
    en = load()
    text = (ROOT / "macOS/Resources/en.lproj/Localizable.strings").read_text(encoding="utf-8")
    assert text == en.render()


# ---- The Web UI: L('…') in web/src and web/server, web/src/i18n/en.json ----

WEB_TABLE = ROOT / "web/src/i18n/en.json"


def web_keys() -> set[str]:
    """The first argument of every L('…') call (single-quoted, as the code writes them)."""
    keys = set()
    for p in [*(ROOT / "web/src").rglob("*.ts"), *(ROOT / "web/src").rglob("*.tsx"), *(ROOT / "web/server").rglob("*.ts")]:
        if "/gen/" in str(p):
            continue
        for m in re.finditer(r"\bL\('((?:[^'\\\n]|\\.)*)'", p.read_text(encoding="utf-8")):
            keys.add(json.loads('"' + m.group(1).replace('"', '\\"').replace("\\'", "'") + '"'))
    # The labels of the controls the bird names (agent.json), looked up with L(label) on the server.
    return keys | set(control_labels())


def control_labels() -> list[str]:
    agent = json.loads((ROOT / "agent/Shared/agent.json").read_text(encoding="utf-8"))
    return [c["label"] for c in agent["guide"]["controls"]]


def test_the_controls_the_bird_names_have_english_in_both_apps():
    # The prompts name no button in any language: each app fills in its own labels.
    web = json.loads(WEB_TABLE.read_text(encoding="utf-8"))
    mac = load().EN
    assert not [l for l in control_labels() if l not in web or l not in mac]


def test_every_web_string_has_its_english():
    table = json.loads(WEB_TABLE.read_text(encoding="utf-8"))
    keys = web_keys()
    assert keys, "no L('…') found"
    assert not sorted(keys - table.keys()), "missing from web/src/i18n/en.json"
    assert not sorted(table.keys() - keys), "no longer used: remove from web/src/i18n/en.json"


def test_web_translations_put_in_the_same_values():
    table = json.loads(WEB_TABLE.read_text(encoding="utf-8"))
    places = lambda s: sorted(re.findall(r"\{\d+\}", s))
    assert not {k: v for k, v in table.items() if places(k) != places(v)}
