"""Plotly figures and Altair charts in the Card's display, as interactive plots.

They go to the UI as a module display (``plotly``: the figure's JSON;
``vega-lite``: the chart's Vega-Lite spec with its data inline), which the app
and the web UI draw with the bundled plotly.js / Vega (no network): zoom,
hover and legends work in the expanded view; on the canvas the plot is drawn
still. Code written for a notebook or a browser works as it is:

* ``fig.show()`` (Plotly): the ``tsugumi`` renderer is the default once
  ``plotly.io`` is imported.
* ``chart.show()`` (Altair): shows the chart here once ``altair`` is imported.
* ``ctx.display_figure(fig_or_chart)`` does the same explicitly.

A module display carries at most 1 MiB of JSON: plot fewer points (or use
matplotlib, whose figures are images) beyond that.
"""

from __future__ import annotations

import importlib.abc
import importlib.machinery
import json
import sys

import _tsugumi_host as _host

LIMIT = 1 << 20
"""The core's limit for a module display's JSON (bytes)."""


def is_plotly(obj) -> bool:
    return type(obj).__module__.startswith("plotly.") and hasattr(obj, "to_plotly_json")


def is_altair(obj) -> bool:
    return type(obj).__module__.startswith("altair.") and hasattr(obj, "to_dict")


def _send(name: str, text: str, what: str) -> None:
    import tsugumi

    tsugumi._shown_by_card()  # a figure matplotlib left open does not replace it
    size = len(text.encode())
    if size > LIMIT:
        raise tsugumi.Failure("output_invalid",
                              f"the {what} is {size / LIMIT:.1f} MiB of JSON; an interactive plot takes at most 1 MiB: "
                              "plot fewer points, or draw it with matplotlib (an image)")
    _host.display_module(name, text)


def show_plotly(fig) -> None:
    """A Plotly figure (or its dict) as the Card's display."""
    import plotly.io as pio

    _send("plotly", pio.to_json(fig, validate=False, pretty=False), "Plotly figure")


def show_altair(chart) -> None:
    """An Altair chart as the Card's display (its data inline in the spec)."""
    _send("vega-lite", json.dumps(chart.to_dict(), separators=(",", ":")), "Altair chart")


# -- hooks: set up plotly and altair right after the Card imports them --

def _setup_plotly(pio) -> None:
    from plotly.io._base_renderers import ExternalRenderer

    class TsugumiRenderer(ExternalRenderer):
        """fig.show() draws in the Card's display."""

        def render(self, fig_dict):
            show_plotly(fig_dict)

    pio.renderers["tsugumi"] = TsugumiRenderer()
    pio.renderers.default = "tsugumi"


def _setup_altair(alt) -> None:
    def show(self) -> None:
        """Shows the chart in the Card's display (Tsugumi)."""
        show_altair(self)

    alt.TopLevelMixin.show = show


_HOOKS = {"plotly.io": _setup_plotly, "altair": _setup_altair}


class _Loader(importlib.abc.Loader):
    """The module's own loader, then its hook."""

    def __init__(self, loader, hook):
        self._loader, self._hook = loader, hook

    def create_module(self, spec):
        return self._loader.create_module(spec)

    def exec_module(self, module):
        self._loader.exec_module(module)
        self._hook(module)

    def __getattr__(self, name):  # resources and the rest, from the real loader
        return getattr(self._loader, name)


class _Finder(importlib.abc.MetaPathFinder):
    def find_spec(self, name, path, target=None):
        hook = _HOOKS.get(name)
        if hook is None:
            return None
        spec = importlib.machinery.PathFinder.find_spec(name, path, target)
        if spec is None or spec.loader is None:
            return None
        spec.loader = _Loader(spec.loader, hook)
        return spec


def install() -> None:
    """Sets up plotly and altair when they are imported (now, if they already are)."""
    if not any(isinstance(f, _Finder) for f in sys.meta_path):
        sys.meta_path.insert(0, _Finder())
    for name, hook in _HOOKS.items():
        if name in sys.modules:
            hook(sys.modules[name])
