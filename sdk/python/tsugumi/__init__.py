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

matplotlib draws into the Card's display (``tsugumi.mpl``): ``plt.show()``,
``plt.pause()`` and animations work as they would in a window. Plotly's
``fig.show()`` and Altair's ``chart.show()`` show interactive plots there
(``tsugumi.plots``).
"""

from __future__ import annotations

import importlib
import os
import sys
from typing import TYPE_CHECKING, Any, Iterable, Optional

import _tsugumi_host as _host

if TYPE_CHECKING:
    import flatbuffers

__all__ = ["Context", "Failure"]

# matplotlib's figures go to the Card's display unless the Card picks a backend.
os.environ.setdefault("MPLBACKEND", "module://tsugumi.mpl")

_in_call = False
"""Inside an invocation or a run-start hook (the host's functions can be called)."""

from tsugumi import plots as _plots  # noqa: E402 - after the module's state, which plots does not read

_plots.install()

_card_displayed = False
"""The Card sent a display itself (ctx.display_*) during this call."""


def _shown_by_card() -> None:
    global _card_displayed
    _card_displayed = True


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
        # Not at the top: it imports numpy, and with it the native libraries that read
        # their settings as they load. The top of the Card's main.py comes before them.
        import flatbuffers

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
        _shown_by_card()
        _host.display_text(text)

    def display_markdown(self, text: str) -> None:
        """Markdown with TeX math ($…$ inline, $$…$$ displayed), shown formatted."""
        _shown_by_card()
        _host.display_markdown(text)

    def display_scalar(self, value: float, label: str) -> None:
        _shown_by_card()
        _host.display_scalar(float(value), label)

    def display_line_chart(self, series: Iterable[dict]) -> None:
        _shown_by_card()
        _host.display_line_chart([(s["id"], s.get("label", s["id"]), list(map(float, s["x"])),
                                   list(map(float, s["y"]))) for s in series])

    def display_table(self, columns: list, rows: Iterable[Iterable]) -> None:
        _shown_by_card()
        _host.display_table([str(c) for c in columns], [[str(c) for c in r] for r in rows])

    def display_image(self, data: bytes, media_type: str = "image/png", alt: str = "") -> None:
        _shown_by_card()
        _host.display_image(media_type, bytes(data), alt)

    def display_points(self, x, y, z, colors=None) -> None:
        _shown_by_card()
        _host.display_points(list(map(float, x)), list(map(float, y)), list(map(float, z)),
                             None if colors is None else [int(c) for c in colors])

    def display_figure(self, *figures) -> None:
        """matplotlib figures as the Card's image (several are stacked), whatever the
        backend; a Plotly figure or an Altair chart as an interactive plot."""
        _shown_by_card()
        if any(_plots.is_plotly(f) or _plots.is_altair(f) for f in figures):
            if len(figures) != 1:
                raise Failure("output_invalid", "display_figure shows one Plotly figure or Altair chart at a time")
            (_plots.show_plotly if _plots.is_plotly(figures[0]) else _plots.show_altair)(figures[0])
            return
        from tsugumi import mpl

        mpl.display(list(figures))

    def display_module(self, module: str, config, blobs=None) -> None:
        _shown_by_card()
        import json
        _host.display_module(module, config if isinstance(config, str) else json.dumps(config), blobs or {})

    # -- control --
    def log(self, message: str) -> None:
        _host.log(message)

    def cancelled(self) -> bool:
        return bool(_host.cancelled())

    def remaining_ms(self) -> int:
        return int(_host.remaining_ms())


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


def _guard(body, report: bool = True) -> bool:
    global _in_call, _card_displayed
    _in_call = report  # report is False only while loading, outside any call
    _card_displayed = False
    done = False
    try:
        if report:
            _adopt_environment()
        body()
        _finish_figures(show=True)
        done = True
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
    finally:
        if not done:
            _finish_figures(show=False)
        _in_call = False


def _finish_figures(show: bool) -> None:
    """With the Tsugumi backend, the figures a call left open are closed (and
    first shown, after a call that succeeded unless the Card sent a display of
    its own: a figure it forgot to close does not replace that)."""
    mod = sys.modules.get("tsugumi.mpl")
    plt = sys.modules.get("matplotlib.pyplot")
    if mod is None or plt is None or not _in_call or "tsugumi.mpl" not in str(plt.get_backend()):
        return
    if show and not _card_displayed:
        mod._finish_invocation()
    else:
        mod._close_all()


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
