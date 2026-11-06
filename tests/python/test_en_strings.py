"""The English UI strings (macOS/Resources/tools/en_strings.py)."""
import importlib.util
import pathlib

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
