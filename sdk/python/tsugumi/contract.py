"""The Card's contract, for code that does not know the port types when it is
written (helpers shared by many Cards): which ports the Card has, what kind
each is, and the connected input as an object of its generated type.

    from tsugumi import contract

    contract.port("data")["kind"]         # "flatbuffer", "file" or "directory"
    row = contract.input_object(inputs, "data")   # the generated <Table>T object
    factor = contract.params_object(inputs).factor

A Card's own main.py knows its ports and imports the generated classes by
name, as the Card guide shows. The SDK installs the contract beside the
generated classes (`_contract.json`); read it through this module, not by path.
"""

from __future__ import annotations

import importlib
import json
from functools import cache
from pathlib import Path
from typing import Any, Optional

from tsugumi import Failure

__all__ = ["ports", "port", "input_class", "input_object", "params_object"]


@cache
def _contract() -> dict:
    return json.loads((Path(__file__).resolve().parent.parent / "_contract.json").read_text())


def ports(side: str = "inputs") -> list[dict]:
    """The typed ports of one side ("inputs" or "outputs"), in order: each has
    ``name``, ``type`` (the generated module, such as ``inputs.data.hello.Number``),
    ``kind`` and ``required``. An input that is not connected has no type yet
    and is not listed."""
    if side not in ("inputs", "outputs"):
        raise ValueError("side is 'inputs' or 'outputs'")
    return [dict(p) for p in _contract()[side]]


def port(name: str, side: str = "inputs") -> Optional[dict]:
    """The port of that name, or None when the Card has none (or it is an unconnected input)."""
    return next((p for p in ports(side) if p["name"] == name), None)


def _generated(name: str, side: str, suffix: str = "") -> Any:
    found = port(name, side)
    if found is None:
        raise Failure("input_missing" if side == "inputs" else "output_invalid", f"this Card has no typed {side[:-1]} {name}")
    return getattr(importlib.import_module(found["type"]), found["type"].rsplit(".", 1)[-1] + suffix)


def input_class(name: str) -> Any:
    """The generated class of an input port, as ``Inputs.input`` takes it."""
    return _generated(name, "inputs")


def input_object(inputs: Any, name: str) -> Any:
    """The input message as an object of its generated ``<Table>T`` class:
    plain attributes, readable without knowing the type's accessors."""
    return _generated(name, "inputs", "T").InitFromObj(inputs.input(name, input_class(name)))


def params_object(inputs: Any) -> Any:
    """The params as an object of the generated ``ParamsT`` class."""
    return inputs.params
