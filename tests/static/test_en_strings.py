"""Check the shared English dictionary and its macOS format."""
import json
from pathlib import Path
import re
import runpy

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "ui/shared/i18n/strings.py"
GENERATOR = runpy.run_path(str(TOOL))
ARGUMENT = GENERATOR["ARGUMENT"]
SPEC = re.compile(r"%(?:(\d+)\$)?((?:ll|l|h)?[@dDuUxXoOfeEgGcCsSpaA])")
MAC_TOKEN = re.compile(rf"%%|{SPEC.pattern}")


def unique_keys(pairs):
    result = dict(pairs)
    assert len(result) == len(pairs), "duplicate translation key"
    return result


def specifiers(text):
    arguments = (match.groups() for match in MAC_TOKEN.finditer(text) if match[2])
    return {
        (int(position) if position else index, kind)
        for index, (position, kind) in enumerate(arguments, 1)
    }


def test_every_translation_takes_the_arguments_of_its_source():
    dictionary = json.loads(
        TOOL.with_name("en.json").read_text(encoding="utf-8"),
        object_pairs_hook=unique_keys,
    )
    for ja, entry in dictionary.items():
        en = entry if isinstance(entry, str) else entry["text"]
        assert set(ARGUMENT.findall(ja)) == set(ARGUMENT.findall(en)), ja
        if isinstance(entry, dict):
            assert ARGUMENT.findall(ja) == [str(i) for i in range(len(entry["mac"]))], ja
            assert all(SPEC.fullmatch("%" + kind) for kind in entry["mac"]), ja

    table = GENERATOR["mac_strings"](dictionary)
    # A reordered integer must not be read as an object by Swift.
    wrong = {ja: en for ja, en in table.items() if specifiers(ja) != specifiers(en)}
    assert not wrong


def test_escaped_percent_does_not_consume_a_mac_argument():
    table = GENERATOR["mac_strings"]({"結果 {0}": {"text": "Result %{0}", "mac": ["@"]}})
    ja, en = next(iter(table.items()))
    assert specifiers(en) == set()
    assert specifiers(ja) == {(1, "@")}
    assert specifiers("%% %@") == specifiers("%@")
