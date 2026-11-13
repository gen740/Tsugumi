"""The C# SDK's handwritten mirror of the Card ABI stays the ABI.

sdk/csharp/Context.cs declares NativeValue, NativeSeries and NativeCall by
hand, in the field order of sdk/include/tsugumi/sdk/abi.h. When a C# Card
loads, sdk/csharp/host.cc passes the compiler's offsetof/sizeof of every abi.h
field and sdk/csharp/Layout.cs compares them with the managed layout (a drift
fails tsugumi_load). These checks need no .NET: they read the sources and
compare field order, field types and callback signatures, and that the two
layout tables (host.cc, Layout.cs) name every field. Appending a callback to
abi.h without the C# side fails here.

Runs with pytest, or alone: python3 tests/python/test_csharp_abi.py
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ABI = ROOT / "sdk/include/tsugumi/sdk/abi.h"
CONTEXT = ROOT / "sdk/csharp/Context.cs"
HOST = ROOT / "sdk/csharp/host.cc"
LAYOUT = ROOT / "sdk/csharp/Layout.cs"

# Every struct the worker hands to a C# Card, with its managed mirror.
MIRRORS = {"tsugumi_value": "NativeValue", "tsugumi_blob": "NativeBlob", "tsugumi_series": "NativeSeries", "tsugumi_call": "NativeCall"}
# C scalar types and their blittable C# types (LP64 and LLP64 alike).
SCALARS = {"void": "void", "char": "byte", "uint8_t": "byte", "uint32_t": "uint", "uint64_t": "ulong",
           "int64_t": "long", "int": "int", "size_t": "nuint", "double": "double", "float": "float", **MIRRORS}


def c_type(text: str) -> str:
    """`const char* const*` -> `byte**`; unknown C types raise."""
    text = re.sub(r"\bconst\b", " ", text)
    stars = text.count("*")
    base = text.replace("*", " ").split()
    assert len(base) == 1 and base[0] in SCALARS, f"no C# mapping for C type {text!r}"
    return SCALARS[base[0]] + "*" * stars


def split_params(text: str) -> list[str]:
    return [p.strip() for p in text.split(",") if p.strip()]


def c_structs(source: str) -> dict[str, list[tuple[str, str]]]:
    """name -> [(field, C# type)] in declaration order."""
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    source = re.sub(r"//[^\n]*", "", source)
    structs = {}
    for name, body, alias in re.findall(r"typedef\s+struct\s+(\w+)\s*\{(.*?)\}\s*(\w+)\s*;", source, re.S):
        assert name == alias, name
        fields = []
        for decl in (d.strip() for d in body.split(";")):
            if not decl:
                continue
            pointer = re.fullmatch(r"(.+?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)", decl, re.S)
            if pointer:
                ret, field, params = pointer.groups()
                # A parameter is `type name`: drop the name.
                types = [c_type(re.sub(r"\w+$", "", p)) for p in split_params(params)]
                fields.append((field, f"delegate* unmanaged[Cdecl]<{', '.join(types + [c_type(ret)])}>"))
            else:
                data = re.fullmatch(r"(.+?)\b(\w+)", decl, re.S)
                assert data, decl
                fields.append((data[2], c_type(data[1])))
        structs[name] = fields
    return structs


def cs_structs(source: str) -> dict[str, list[tuple[str, str]]]:
    """name -> [(field, normalized type)] in declaration order."""
    structs = {}
    for name, body in re.findall(r"\[StructLayout\(LayoutKind\.Sequential\)\]\s*internal\s+unsafe\s+struct\s+(\w+)\s*\{(.*?)\n\}", source, re.S):
        fields = []
        for decl in (d.strip() for d in body.split(";")):
            if not decl:
                continue
            field = re.fullmatch(r"internal\s+(.+?)\s+(\w+)", decl, re.S)
            assert field, decl
            kind = re.sub(r"\s+", " ", field[1]).replace(" *", "*")
            fields.append((field[2], kind))
        structs[name] = fields
    return structs


def mirror_errors(abi: str, context: str) -> list[str]:
    native, managed = c_structs(abi), cs_structs(context)
    errors = []
    for c_name, cs_name in MIRRORS.items():
        a, b = native.get(c_name, []), managed.get(cs_name, [])
        if not a or not b:
            errors.append(f"{c_name} or {cs_name} not found")
            continue
        for i in range(max(len(a), len(b))):
            left = f"{c_name}.{a[i][0]} ({a[i][1]})" if i < len(a) else "nothing"
            right = f"{cs_name}.{b[i][0]} ({b[i][1]})" if i < len(b) else "nothing"
            if i >= len(a) or i >= len(b) or a[i][1] != b[i][1]:
                errors.append(f"field {i}: abi.h has {left}, Context.cs has {right}")
    return errors


def test_managed_structs_mirror_abi_h():
    assert not mirror_errors(ABI.read_text(), CONTEXT.read_text())


def test_appending_a_callback_without_csharp_is_detected():
    """The check itself: a callback appended to tsugumi_call only."""
    abi = ABI.read_text().replace(
        "} tsugumi_call;", "  int (*display_progress)(void* host, double fraction);\n} tsugumi_call;")
    assert abi != ABI.read_text()
    errors = mirror_errors(abi, CONTEXT.read_text())
    assert errors and "display_progress" in errors[0], errors
    # A matching C# field makes it whole again.
    context = CONTEXT.read_text().replace(
        "DisplayJson;\n}", "DisplayJson;\n    internal delegate* unmanaged[Cdecl]<void*, double, int> Progress;\n}")
    assert not mirror_errors(abi, context)


def test_a_changed_signature_is_detected():
    """Same size and offset, different meaning: only the signature check sees it."""
    abi = ABI.read_text().replace("int64_t (*remaining_ms)", "uint64_t (*remaining_ms)")
    assert abi != ABI.read_text()
    errors = mirror_errors(abi, CONTEXT.read_text())
    assert len(errors) == 1 and "remaining_ms" in errors[0], errors


def test_load_time_layout_tables_name_every_field():
    """host.cc's k_layout and Layout.cs list the same fields as abi.h, in its
    order, and Layout.cs measures each one at the managed field of the same
    position."""
    native, managed = c_structs(ABI.read_text()), cs_structs(CONTEXT.read_text())
    expected = []
    for c_name in MIRRORS:
        expected += [c_name] + [f"{c_name}.{field}" for field, _ in native[c_name]]
    host = []
    for kind, args in re.findall(r"TSUGUMI_(STRUCT|FIELD)\(([^)]*)\),", HOST.read_text()):
        parts = [p.strip() for p in args.split(",")]
        host.append(".".join(parts))
    assert host == expected

    layout = LAYOUT.read_text()
    listed = re.findall(r'list\.Add\(\("(\w+)", 0,|Add\("([\w.]+)", \w+, &\w+->(\w+),', layout)
    names = [whole or field for whole, field, _ in listed]
    assert names == expected
    for whole, field, member in listed:
        if field:
            c_name, c_field = field.split(".")
            index = [f for f, _ in native[c_name]].index(c_field)
            assert managed[MIRRORS[c_name]][index][0] == member, (field, member)


if __name__ == "__main__":
    for name, test in sorted(globals().items()):
        if name.startswith("test_"):
            test()
            print("ok", name)
