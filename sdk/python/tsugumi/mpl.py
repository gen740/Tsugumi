"""A matplotlib backend that sends figures to the Card's display.

Python Cards use it unless they choose another backend (``MPLBACKEND`` is set
to ``module://tsugumi.mpl`` before the Card is imported), so a script written
for a window works as it is, the way X forwarding brings a remote window to
the screen in front of you:

* ``plt.show()`` draws the open figures into the Card's display (several
  figures are stacked into one image) and closes them, as closing the window
  would.
* ``plt.pause(t)``, ``fig.canvas.draw_idle()`` and ``fig.canvas.flush_events()``
  update the display while the Card keeps running, so a loop that redraws a
  figure is seen live. Frames come at most ``FPS`` times a second; the last
  one is always sent.
* Timers run (``fig.canvas.new_timer``), so ``matplotlib.animation.FuncAnimation``
  plays in the display while ``plt.show()`` waits for it: until the animation
  ends, or until the run is stopped or reaches its deadline (closing its
  window, in a GUI).
* Figures still open when ``main`` returns are shown (unless the Card sent a
  display of its own with ``ctx.display_*``) and closed, so a Card that only
  draws needs no ``show()``, and figures do not pile up in the worker, which
  stays across invocations and runs.
"""

from __future__ import annotations

import io
import time

import _tsugumi_host as _host
import tsugumi as _sdk
from matplotlib._pylab_helpers import Gcf
from matplotlib.backend_bases import FigureManagerBase, TimerBase, _Backend
from matplotlib.backends.backend_agg import FigureCanvasAgg

FPS = 15
"""Live frames per second at most (the last frame is always sent)."""

SCALE = 2
"""Pixels per figure pixel in the image (2 for a sharp picture on Retina screens)."""

LIVE_SCALE = 1
"""The same for the frames of a live figure (each frame costs less to draw); its
last frame is sent at SCALE when the call ends."""

def _add_fonts() -> None:
    """The Card's fallback fonts (``_fonts.txt``, written by pythonCard: a Japanese
    font when the Card uses matplotlib) go after the font families, so Japanese
    labels draw (matplotlib takes a glyph its first font lacks from the next
    family) while Latin text keeps matplotlib's own look."""
    from pathlib import Path

    from matplotlib import font_manager, rcParams

    listing = Path(__file__).with_name("_fonts.txt")
    if not listing.is_file():
        return
    names = []
    for line in listing.read_text().splitlines():
        if line.strip():
            font_manager.fontManager.addfont(line.strip())
            names.append(font_manager.FontProperties(fname=line.strip()).get_name())
    # Gothic (sans-serif) faces before Mincho (serif) ones, matching matplotlib's sans-serif look.
    names = sorted(dict.fromkeys(names), key=lambda n: any(w in n for w in ("Mincho", "Serif")))
    family = list(rcParams["font.family"])
    rcParams["font.family"] = family + [n for n in names if n not in family]


_add_fonts()

_timers: set["TimerTsugumi"] = set()
_last_sent = 0.0
_pending = False  # a frame is waiting for the next FPS slot


def _stopped() -> bool:
    """The run was stopped, or its deadline is near (or there is no call to wait in)."""
    if not _sdk._in_call:
        return True
    return bool(_host.cancelled()) or 0 <= int(_host.remaining_ms()) < 200


def figure_png(figure, scale: float = SCALE, tight: bool = True) -> bytes:
    """The figure as PNG bytes, ``scale`` times its pixel size; ``tight`` crops it
    to its content (live frames keep the figure's size, so they do not jump)."""
    buf = io.BytesIO()
    figure.savefig(buf, format="png", dpi=figure.dpi * scale, bbox_inches="tight" if tight else None,
                   facecolor=figure.get_facecolor())
    return buf.getvalue()


def _stack(images: list[bytes]) -> bytes:
    """Several PNGs one above the other, on white."""
    if len(images) == 1:
        return images[0]
    from PIL import Image  # matplotlib depends on Pillow

    pics = [Image.open(io.BytesIO(b)).convert("RGBA") for b in images]
    gap = 16 * SCALE
    w = max(p.width for p in pics)
    h = sum(p.height for p in pics) + gap * (len(pics) - 1)
    out = Image.new("RGBA", (w, h), (255, 255, 255, 255))
    y = 0
    for p in pics:
        out.paste(p, ((w - p.width) // 2, y), p)
        y += p.height + gap
    buf = io.BytesIO()
    out.save(buf, format="PNG")
    return buf.getvalue()


def display(figures=None, live: bool = False) -> None:
    """Sends ``figures`` (default: every open pyplot figure) as the Card's image now;
    ``live`` for a frame of a figure that keeps changing."""
    global _last_sent, _pending
    if figures is None:
        figures = [m.canvas.figure for m in Gcf.get_all_fig_managers()]
    figures = [f for f in figures if f is not None]
    if not figures or not _sdk._in_call:
        return
    for f in figures:
        f._tsugumi_live = live or getattr(f, "_tsugumi_live", False)
    data = _stack([figure_png(f, LIVE_SCALE if live else SCALE, tight=not f._tsugumi_live) for f in figures])
    alt = "; ".join(t for t in (f.get_suptitle() or _first_title(f) for f in figures) if t) or "matplotlib figure"
    _host.display_image("image/png", data, alt)
    for f in figures:
        f.stale = False
        f._tsugumi_final = not live
    _last_sent = time.monotonic()
    _pending = False


def _first_title(figure) -> str:
    for ax in figure.axes:
        if ax.get_title():
            return ax.get_title()
    return ""


def _frame(force: bool = False) -> None:
    """A live frame: sent now if the FPS slot is free (or ``force``), else kept for later."""
    global _pending
    if not Gcf.get_all_fig_managers():
        return
    if force or time.monotonic() - _last_sent >= 1.0 / FPS:
        display(live=not force)
    else:
        _pending = True


def _run_loop(until: float | None) -> None:
    """Runs the timers and sends frames until ``until`` (monotonic), until no timer
    is left (when ``until`` is None), or until the run is stopped."""
    while not _stopped():
        now = time.monotonic()
        if until is not None and now >= until:
            break
        if until is None and not _timers:
            break
        due = min((t._due for t in _timers), default=None)
        if due is not None and due <= now:
            timer = min(_timers, key=lambda t: t._due)
            timer._due = now + max(timer.interval, 1) / 1000.0
            if timer.single_shot:
                timer.stop()
            timer._on_timer()
            if any(m.canvas.figure.stale for m in Gcf.get_all_fig_managers()):
                _frame()
            continue
        if _pending and time.monotonic() - _last_sent >= 1.0 / FPS:
            display(live=True)
        next_at = min(x for x in (due, until, now + 0.02) if x is not None)
        time.sleep(max(0.0, min(next_at - time.monotonic(), 0.02)))
    # A frame still waiting goes with the next one, or when the call ends.


class TimerTsugumi(TimerBase):
    """A timer driven by the loop in ``plt.show()`` / ``plt.pause()``."""

    def __init__(self, *args, **kwargs):
        self._due = 0.0
        super().__init__(*args, **kwargs)

    def _timer_start(self):
        self._due = time.monotonic() + max(self.interval, 1) / 1000.0
        _timers.add(self)

    def _timer_stop(self):
        _timers.discard(self)

    def _timer_set_interval(self):
        if self in _timers:
            self._due = time.monotonic() + max(self.interval, 1) / 1000.0


class FigureCanvasTsugumi(FigureCanvasAgg):
    _timer_cls = TimerTsugumi
    # Each frame is the whole figure (an image), so animations draw without blitting.
    supports_blit = False

    def draw_idle(self, *args, **kwargs):
        # The frame draws the figure itself: no separate draw here.
        _frame()

    def flush_events(self):
        if _pending and time.monotonic() - _last_sent >= 1.0 / FPS:
            display(live=True)

    def start_event_loop(self, timeout=0):
        _run_loop(time.monotonic() + timeout if timeout > 0 else None)


def _drawn() -> None:
    """Draws each figure once on its canvas, as a window would when it opens: an
    animation starts on that first draw (not on the draws that save it)."""
    for m in Gcf.get_all_fig_managers():
        if not getattr(m.canvas, "_tsugumi_drawn", False):
            m.canvas._tsugumi_drawn = True
            m.canvas.draw()


class FigureManagerTsugumi(FigureManagerBase):
    def show(self):
        _drawn()
        _frame(force=True)


FigureCanvasTsugumi.manager_class = FigureManagerTsugumi


@_Backend.export
class _BackendTsugumi(_Backend):
    FigureCanvas = FigureCanvasTsugumi
    FigureManager = FigureManagerTsugumi

    @staticmethod
    def show(*, block=None):
        if not Gcf.get_all_fig_managers():
            return
        _drawn()
        if block is False:  # plt.pause, plt.show(block=False): a live frame, the figures stay
            _frame()
            return
        # Blocking, as a window would until it is closed: the animations play out.
        display(live=bool(_timers))
        _run_loop(None)
        _final()
        _close_all()


def _close_all() -> None:
    import matplotlib.pyplot as plt

    plt.close("all")
    _timers.clear()


def _final() -> None:
    """The open figures at full size, unless that is what was sent last."""
    global _pending
    figures = [m.canvas.figure for m in Gcf.get_all_fig_managers()]
    if _pending or any(f.stale or not getattr(f, "_tsugumi_final", False) for f in figures):
        display(figures)
    _pending = False


def _finish_invocation() -> None:
    """After ``main``: figures still open are shown and closed."""
    _final()
    _close_all()
