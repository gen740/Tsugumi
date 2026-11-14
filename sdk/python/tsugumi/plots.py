"""Plotly and Altair serialization used exclusively by Report.figure."""

from __future__ import annotations

import json

import _tsugumi_host as _host

LIMIT = 1 << 20
"""The core's limit for a module display's JSON (bytes)."""


def is_plotly(obj) -> bool:
    return type(obj).__module__.startswith("plotly.") and hasattr(obj, "to_plotly_json")


def is_altair(obj) -> bool:
    return type(obj).__module__.startswith("altair.") and hasattr(obj, "to_dict")


def _send(name: str, text: str, what: str) -> None:
    import tsugumi

    size = len(text.encode())
    if size > LIMIT:
        raise tsugumi.Failure("output_invalid",
                              f"the {what} is {size / LIMIT:.1f} MiB of JSON; an interactive plot takes at most 1 MiB: "
                              "plot fewer points, or draw it with matplotlib (an image)")
    _host.display_module(name, text, {})


def show_plotly(fig) -> None:
    """A Plotly figure (or its dict) as the Card's display."""
    import plotly.io as pio

    _send("plotly", pio.to_json(fig, validate=False, pretty=False), "Plotly figure")


def show_altair(chart) -> None:
    """An Altair chart as the Card's display (its data inline in the spec)."""
    _send("vega-lite", json.dumps(chart.to_dict(), separators=(",", ":")), "Altair chart")
