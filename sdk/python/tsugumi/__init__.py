"""Tsugumi Python Card SDK.

A Python Card provides ``main(ctx)`` in ``src/main.py``::

    from inputs.value.collatz.Number import Number
    import outputs.next.collatz.Number as out

    def main(ctx):
        n = ctx.input("value", Number).Value()
        b = ctx.builder()
        out.Start(b); out.AddValue(b, n // 2)
        ctx.output("next", b, out.End(b))

Input and output classes are generated from the Card's ``_inputs.fbs`` and
``_outputs.fbs`` (namespaces ``inputs.<port>`` and ``outputs.<port>``); the
parameters are ``params.Params``.
"""

from __future__ import annotations

import importlib
import sys
from typing import Any, Iterable, Optional

import _tsugumi_host as _host
import flatbuffers

__all__ = ["Context", "Failure"]


class Failure(Exception):
    """Fails the invocation with a FailureKind name (e.g. ``output_invalid``)."""

    def __init__(self, kind: str, message: str):
        super().__init__(message)
        self.kind = kind
        self.message = message


class Context:
    def __init__(self) -> None:
        self._inputs = {name: (kind, data) for name, kind, data in _host.inputs()}
        self._info = _host.info()

    # -- identity and directories --
    @property
    def run_id(self) -> str:
        return self._info["run_id"]

    @property
    def card(self) -> str:
        return self._info["card"]

    @property
    def invocation(self) -> int:
        return self._info["invocation"]

    @property
    def working_directory(self) -> str:
        return self._info["working_directory"]

    @property
    def temporary_directory(self) -> str:
        return self._info["temporary_directory"]

    @property
    def output_directory(self) -> str:
        return self._info["output_directory"]

    # -- inputs and params --
    def has_input(self, port: str) -> bool:
        return port in self._inputs

    def input_bytes(self, port: str) -> bytes:
        if port not in self._inputs:
            raise Failure("input_missing", f"input {port} was not provided")
        return self._inputs[port][1]

    def input(self, port: str, cls: Any) -> Any:
        """Returns the input message as the generated class ``cls``."""
        return cls.GetRootAs(self.input_bytes(port), 0)

    def params(self) -> Any:
        from params.Params import Params  # generated from _params.fbs

        return Params.GetRootAs(_host.params(), 0)

    # -- outputs --
    def builder(self, size: int = 1024) -> flatbuffers.Builder:
        return flatbuffers.Builder(size)

    def output(self, port: str, builder: flatbuffers.Builder, root: Optional[int] = None) -> None:
        if root is not None:
            builder.Finish(root)
        if not _host.emit_output(port, bytes(builder.Output())):
            raise Failure("output_invalid", f"output {port} was rejected")

    def output_bytes(self, port: str, data: bytes) -> None:
        if not _host.emit_output(port, data):
            raise Failure("output_invalid", f"output {port} was rejected")

    # -- display --
    def display_text(self, text: str) -> None:
        _host.display_text(text)

    def display_markdown(self, text: str) -> None:
        """Markdown with TeX math ($…$ inline, $$…$$ displayed), shown formatted."""
        _host.display_markdown(text)

    def display_scalar(self, value: float, label: str) -> None:
        _host.display_scalar(float(value), label)

    def display_line_chart(self, series: Iterable[dict]) -> None:
        _host.display_line_chart([(s["id"], s.get("label", s["id"]), list(map(float, s["x"])),
                                   list(map(float, s["y"]))) for s in series])

    def display_table(self, columns: list, rows: Iterable[Iterable]) -> None:
        _host.display_table([str(c) for c in columns], [[str(c) for c in r] for r in rows])

    def display_image(self, data: bytes, media_type: str = "image/png", alt: str = "") -> None:
        _host.display_image(media_type, bytes(data), alt)

    def display_points(self, x, y, z, colors=None) -> None:
        _host.display_points(list(map(float, x)), list(map(float, y)), list(map(float, z)),
                             None if colors is None else [int(c) for c in colors])

    def display_module(self, name: str, data) -> None:
        import json
        _host.display_module(name, data if isinstance(data, str) else json.dumps(data))

    # -- control --
    def log(self, message: str) -> None:
        _host.log(message)

    def cancelled(self) -> bool:
        return bool(_host.cancelled())

    def remaining_ms(self) -> int:
        return int(_host.remaining_ms())


_card_module = None


def _guard(body, report: bool = True) -> bool:
    try:
        body()
        sys.stdout.flush()
        sys.stderr.flush()
        return True
    except Failure as f:
        if report:
            _host.fail(f.kind, f.message)
        else:
            print(f"{f.kind}: {f.message}", file=sys.stderr)
        return False
    except Exception as e:  # noqa: BLE001 - every error becomes a structured failure
        import traceback

        traceback.print_exc()
        sys.stderr.flush()
        if report:  # while loading there is no invocation to fail
            _host.fail("worker_failed", f"{type(e).__name__}: {e}")
        return False


def _load() -> bool:
    """Imports card.main when the worker starts (before any invocation)."""
    def body():
        global _card_module
        _card_module = importlib.import_module("card.main")
    return _guard(body, report=False)


def _run_start() -> bool:
    """Before the first invocation of each run: card.main.on_run_start(ctx), if defined."""
    hook = getattr(_card_module, "on_run_start", None)
    return _guard(lambda: hook(Context())) if hook else True


def _invoke() -> bool:
    return _guard(lambda: _card_module.main(Context()))
