"""Tsugumi Python Card SDK.

A Python Card provides ``main(inputs[, report[, run]])`` in ``src/main.py``::

    def main(inputs, report):
        n = inputs.value.value
        report.text("halved")
        return {"next": {"value": n // 2}}

Inputs are read-only. Returned dictionaries, sequences, arrays and data frames
are encoded according to the generated output contract, inside the SDK.

All presentation goes through Report, including ``report.figure(fig)`` for
matplotlib, Plotly and Altair. ``report.figure()`` shows open matplotlib
figures and plays animations; ``report.figure(wait=seconds)`` publishes a live
frame and drives timers for that interval. Drawing alone does not publish.
"""

from __future__ import annotations

import importlib
import os
import sys
import threading
from typing import TYPE_CHECKING, Any, Iterable, Optional

import _tsugumi_host as _host

if TYPE_CHECKING:
    import flatbuffers

__all__ = ["Inputs", "Report", "Run", "Failure", "builder", "message"]

# matplotlib's figures go to the Card's display unless the Card picks a backend.
os.environ.setdefault("MPLBACKEND", "module://tsugumi.mpl")

_in_call = False
_active_scope = None
"""Inside an invocation or a run-start hook (the host's functions can be called)."""

from tsugumi import plots as _plots  # noqa: E402 - after the module's state, which plots does not read

_reporting = False

class Failure(Exception):
    """Fails the invocation with a FailureKind name (e.g. ``output_invalid``)."""

    def __init__(self, kind: str, message: str):
        super().__init__(message)
        self.kind = kind
        self.message = message


class Run:
    def __init__(self):
        self._info = _host.info()
        self._scope = _active_scope
        self._owner = threading.get_ident()

    def _check(self):
        if self._scope is None or self._scope is not _active_scope or self._owner != threading.get_ident():
            raise Failure("worker_failed", "Card call has expired or belongs to another thread")

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

    def cancelled(self) -> bool:
        self._check()
        return bool(_host.cancelled())

    def remaining_ms(self) -> int:
        self._check()
        return int(_host.remaining_ms())


def builder(size=1024):
    # Import after the Card source has configured native libraries.
    import flatbuffers
    return flatbuffers.Builder(size)


def message(value, root=None):
    if hasattr(value, "Pack"):
        b = builder()
        b.Finish(value.Pack(b))
        return bytes(b.Output())
    if root is not None:
        value.Finish(root)
    return bytes(value.Output()) if hasattr(value, "Output") else bytes(value)


class _ReadOnly:
    """Read-only attributes of a generated object, including future input kinds."""
    __slots__ = ("_value",)

    def __init__(self, value):
        object.__setattr__(self, "_value", value)

    def __setattr__(self, name, value):
        raise AttributeError("inputs are read-only")

    def __getattr__(self, name):
        return _freeze(getattr(self._value, name))


def _freeze(value):
    if isinstance(value, (list, tuple)):
        return tuple(map(_freeze, value))
    if hasattr(value, "Pack"):
        return _ReadOnly(value)
    if hasattr(value, "setflags"):
        value.setflags(write=False)
    return value


class Inputs:
    __slots__ = ("_values", "_params")

    def __init__(self) -> None:
        object.__setattr__(self, "_values", {name: (kind, bytes(data)) for name, kind, data in _host.inputs()})
        from params.Params import ParamsT
        object.__setattr__(self, "_params", _freeze(ParamsT.InitFromPackedBuf(_host.params())))

    def __setattr__(self, name, value):
        raise AttributeError("inputs are read-only")

    @property
    def params(self):
        return self._params

    def __getattr__(self, name):
        from tsugumi import contract
        port = contract.port(name)
        if port is not None and not port["required"] and not self.has_input(name):
            return None
        return _freeze(contract.input_object(self, name))

    def has_input(self, port: str) -> bool:
        return port in self._values

    def input_bytes(self, port: str) -> bytes:
        if port not in self._values:
            raise Failure("input_missing", f"input {port} was not provided")
        return self._values[port][1]

    def input(self, port: str, cls: Any) -> Any:
        """Read a FlatBuffers view when the low-level generated API is needed."""
        return cls.GetRootAs(self.input_bytes(port), 0)


class Report:
    # -- display --
    def text(self, text: str) -> None:
        _host.display_text(text)

    def markdown(self, text: str) -> None:
        """Markdown with TeX math ($…$ inline, $$…$$ displayed), shown formatted."""
        _host.display_markdown(text)

    def scalar(self, value: float, label: str) -> None:
        _host.display_scalar(float(value), label)

    def line_chart(self, series: Iterable[dict]) -> None:
        _host.display_line_chart([(s["id"], s.get("label", s["id"]), list(map(float, s["x"])),
                                   list(map(float, s["y"]))) for s in series])

    def table(self, columns: list, rows: Iterable[Iterable]) -> None:
        _host.display_table([str(c) for c in columns], [[str(c) for c in r] for r in rows])

    def image(self, data: bytes, media_type: str = "image/png", alt: str = "") -> None:
        _host.display_image(media_type, bytes(data), alt)

    def points(self, x, y, z, colors=None) -> None:
        _host.display_points(list(map(float, x)), list(map(float, y)), list(map(float, z)),
                             None if colors is None else [int(c) for c in colors])

    def figure(self, *figures, wait: Optional[float] = None) -> None:
        """matplotlib figures as the Card's image (several are stacked), whatever the
        backend; a Plotly figure or an Altair chart as an interactive plot."""
        if any(_plots.is_plotly(f) or _plots.is_altair(f) for f in figures):
            if len(figures) != 1:
                raise Failure("output_invalid", "figure shows one Plotly figure or Altair chart at a time")
            (_plots.show_plotly if _plots.is_plotly(figures[0]) else _plots.show_altair)(figures[0])
            return
        from tsugumi import mpl

        global _reporting
        _reporting = True
        try:
            if wait is not None:
                import matplotlib.pyplot as plt
                plt.pause(wait)
            elif figures:
                mpl.display(list(figures))
            else:
                mpl._BackendTsugumi.show()
        finally:
            _reporting = False

    def module(self, module: str, config, blobs=None) -> None:
        import json
        _host.display_module(module, config if isinstance(config, str) else json.dumps(config), blobs or {})

    # -- control --
    def log(self, message: str) -> None:
        _host.log(message)



def _call(function):
    import inspect
    count = len(inspect.signature(function).parameters)
    if count not in (1, 2, 3):
        raise TypeError("Card functions take inputs, optionally report and run")
    args = [Inputs()]
    if count >= 2:
        args.append(Report())
    if count == 3:
        args.append(Run())
    return function(*args)


def _outputs(result):
    """Emit returned batches in order, including a generator's final result."""
    from collections.abc import Mapping
    from tsugumi.values import encode
    if result is None:
        return
    if isinstance(result, Mapping):
        for port, value in result.items():
            if not _host.emit_output(port, encode(port, value)):
                raise Failure("output_invalid", f"output {port} was rejected")
        return
    stream = iter(result)
    while True:
        try:
            batch = next(stream)
        except StopIteration as end:
            if end.value is not None:
                _outputs(end.value)
            break
        _outputs(batch)


_card_module = None


def _adopt_environment() -> None:
    """os.environ is a copy from when the interpreter started, and the worker
    sets the real environment for each call (preRun's exports among it)."""
    current = dict(_host.environ())
    for name in os.environb.keys() - current.keys():
        del os.environb[name]
    for name, value in current.items():
        if os.environb.get(name) != value:
            os.environb[name] = value


def _guard(body) -> bool:
    global _in_call, _active_scope
    _active_scope = object()
    _in_call = True
    done = False
    try:
        _adopt_environment()
        body()
        _close_figures()
        done = True
        sys.stdout.flush()
        sys.stderr.flush()
        return True
    except Failure as f:
        _host.fail(f.kind, f.message)
        return False
    except Exception as e:  # noqa: BLE001 - every error becomes a structured failure
        import traceback

        traceback.print_exc()
        sys.stderr.flush()
        _host.fail("worker_failed", f"{type(e).__name__}: {e}")
        return False
    finally:
        if not done:
            _close_figures()
        _active_scope = None
        _in_call = False


def _close_figures() -> None:
    """Close headless figures at the invocation boundary without publishing."""
    mod = sys.modules.get("tsugumi.mpl")
    plt = sys.modules.get("matplotlib.pyplot")
    if mod is None or plt is None or not _in_call or "tsugumi.mpl" not in str(plt.get_backend()):
        return
    mod._close_all()


def _load() -> bool:
    """Imports card.main inside the first invocation."""
    def body():
        global _card_module
        _card_module = importlib.import_module("card.main")
    return _guard(body)


def _run_start() -> bool:
    """Before the first invocation of each run: card.main.on_run_start(inputs[, report[, run]]), if defined."""
    hook = getattr(_card_module, "on_run_start", None)
    return _guard(lambda: _outputs(_call(hook))) if hook else True


def _invoke() -> bool:
    def body():
        _outputs(_call(_card_module.main))
        if not _host.finish_outputs():
            raise Failure("output_invalid", "required outputs were not emitted")
    return _guard(body)
