"""Small pieces for Visualization Cards: read the connected data as rows, and
save the figure's files beside the data that was drawn.

The plotting itself is in each Card's own src/main.py, where it can be read
and changed. Nothing here chooses columns, filters values or draws.
"""
from __future__ import annotations

import base64
import csv
import json
from pathlib import Path

from tsugumi import Failure, contract

__all__ = ["input_rows", "figure_directory", "embed_font", "emit_figure"]

MAX_ROWS = 1_000_000
MAX_FILE_BYTES = 64 * 1024 * 1024


def _fail(message):
    raise Failure("validation_failed", message)


def _check(ctx):
    if ctx.cancelled():
        raise Failure("cancelled", "Visualization cancelled")
    if ctx.remaining_ms() == 0:
        raise Failure("timeout", "Visualization deadline reached")


def _plain(value):
    if isinstance(value, bytes):
        return value.decode("utf-8")
    if isinstance(value, (list, tuple)):
        return [_plain(v) for v in value]
    if hasattr(value, "tolist"):
        return _plain(value.tolist())
    if hasattr(value, "__dict__"):
        return {k: _plain(v) for k, v in vars(value).items()}
    return value


def input_rows(ctx, field="", member="data.csv"):
    """The connected `data` port as a list of row dictionaries.

    A typed input is read through its generated type. A File input is UTF-8
    CSV or JSON; a Directory input reads its `member`. `field` is a
    dot-separated path to the rows (a list of tables) or to equally sized
    column arrays inside the value."""
    port = contract.port("data")
    if port is None:
        _fail("Connect an upstream output to data before running this Visualization Card")
    value = _plain(contract.input_object(ctx, "data"))
    if port["kind"] != "flatbuffer":
        source = Path(value["value"])
        if port["kind"] == "directory":
            candidate = (source / member).resolve()
            if not member or Path(member).is_absolute() or not candidate.is_relative_to(source.resolve()):
                _fail("file must be a relative member of the input directory")
            source = candidate
        if source.stat().st_size > MAX_FILE_BYTES:
            _fail("Input file exceeds 64 MiB; aggregate it upstream")
        if source.suffix.lower() == ".csv":
            with source.open(newline="", encoding="utf-8-sig") as f:
                rows = []
                for row in csv.DictReader(f):
                    if len(rows) >= MAX_ROWS:
                        _fail("Input exceeds 1,000,000 rows; aggregate it upstream")
                    if len(rows) % 1024 == 0:
                        _check(ctx)
                    rows.append(row)
            value = rows
        elif source.suffix.lower() == ".json":
            value = json.loads(source.read_text())
        else:
            _fail("File inputs must be UTF-8 CSV or JSON; read another format in the Card's own source")
    for part in field.split(".") if field else []:
        if not isinstance(value, dict) or part not in value:
            _fail(f"data_path does not contain {part!r}")
        value = value[part]
    if isinstance(value, dict):
        columns = {k: v for k, v in value.items() if isinstance(v, list)}
        if not columns or any(len(v) > MAX_ROWS for v in columns.values()) or len({len(v) for v in columns.values()}) != 1:
            _fail("Choose data_path containing rows or equally sized column arrays")
        value = [dict(zip(columns, row)) for row in zip(*columns.values())]
    if not isinstance(value, list) or len(value) > MAX_ROWS or not all(isinstance(row, dict) for row in value):
        _fail("Data must be a row table or equally sized column arrays (at most 1,000,000 rows)")
    _check(ctx)
    return value


def figure_directory(ctx):
    """The directory this invocation's `figure` output is made of (created empty)."""
    folder = Path(ctx.output_directory) / "figure"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


def embed_font(html):
    """Put the Card's pinned font (`fonts` in default.nix) into a saved HTML
    figure as the family "TsugumiFigure", so it reads the same offline."""
    html = Path(html)
    listed = Path(__file__).with_name("_fonts.txt")
    fonts = [Path(p) for p in listed.read_text().splitlines() if p] if listed.exists() else []
    font = next((p for p in fonts if "gothic" in str(p).lower() or "ipaexg" in str(p).lower()), None)
    if font is None:
        return
    data = base64.b64encode(font.read_bytes()).decode()
    style = "<style>@font-face{font-family:TsugumiFigure;src:url(data:font/ttf;base64," + data + ")}</style>"
    html.write_text(html.read_text(encoding="utf-8").replace("<head>", "<head>" + style, 1), encoding="utf-8")


def emit_figure(ctx, folder, plotted, **facts):
    """Save what was drawn beside the figure files, then send `folder` as the
    `figure` Directory output.

    `plotted` (row dictionaries) becomes plotted.csv: the values behind the
    marks, not a copy of the whole input (the invocation's recorded inputs keep
    that). `facts` go to metadata.json with the run, Card and invocation."""
    folder = Path(folder)
    columns = list(dict.fromkeys(k for row in plotted for k in row))
    with (folder / "plotted.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        writer.writerows(plotted)
    metadata = {**facts, "run_id": ctx.run_id, "card": ctx.card, "invocation": ctx.invocation}
    (folder / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    _check(ctx)
    import outputs.figure.visualization.Files as output
    b = ctx.builder()
    value = b.CreateString(str(folder))
    output.Start(b)
    output.AddValue(b, value)
    ctx.output("figure", b, output.End(b))
