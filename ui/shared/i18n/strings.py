"""Render the English macOS strings table from the shared UI dictionary.

Use --check DIR with Swift's -emit-localized-strings output to check coverage.
"""
import argparse
import json
from pathlib import Path
import re
import sys

ARGUMENT = re.compile(r"\{(\d+)\}")
DICTIONARY = Path(__file__).with_name("en.json")


def mac_format(text: str, types: list[str], *, positional: bool = False) -> str:
    return ARGUMENT.sub(
        lambda m: "%" + (f"{int(m[1]) + 1}$" if positional else "") + types[int(m[1])],
        text,
    )


def mac_strings(dictionary: dict) -> dict[str, str]:
    table = {}
    for ja, entry in dictionary.items():
        if isinstance(entry, dict):
            en, types = entry["text"], entry["mac"]
            source = mac_format(ja, types)
            order = [int(index) for index in ARGUMENT.findall(en)]
            table[source] = mac_format(
                en, types, positional=order != list(range(len(types)))
            )
        elif not ARGUMENT.search(ja):
            table[ja] = entry
    return table


def render(table: dict[str, str]) -> str:
    def quote(text):
        return json.dumps(text, ensure_ascii=False)

    return "/* Generated from ui/shared/i18n/en.json. */\n" + "".join(
        f"{quote(ja)} = {quote(en)};\n" for ja, en in sorted(table.items())
    )


def missing(directory: Path, table: dict[str, str]) -> list[str]:
    keys = set()
    for path in directory.glob("*.stringsdata"):
        data = json.loads(path.read_text(encoding="utf-8"))
        # Incremental Swift builds can leave output for deleted source files.
        source = Path(data["source"])
        if source.is_absolute() and not source.is_file():
            continue
        for entries in data.get("tables", {}).values():
            keys.update(entry["key"] for entry in entries)
    japanese = {key for key in keys if re.search(r"[\u3040-\u30ff\u4e00-\u9fff]", key)}
    if not japanese:
        raise ValueError(f"No Japanese localization keys extracted from {directory}")
    return sorted(japanese - table.keys())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()
    table = mac_strings(json.loads(DICTIONARY.read_text(encoding="utf-8")))
    if args.check is not None:
        try:
            keys = missing(args.check, table)
        except ValueError as error:
            parser.exit(1, f"{error}\n")
        for key in keys:
            print(key)
        return int(bool(keys))
    sys.stdout.write(render(table))
    return 0


if __name__ == "__main__":
    sys.exit(main())
