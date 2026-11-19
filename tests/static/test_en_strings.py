"""The English UI strings (ui/macos/Resources/tools/en_strings.py)."""
import importlib.util
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "ui/macos/Resources/tools/en_strings.py"


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
