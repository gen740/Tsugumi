"""Runs demos for real with a host tsugumid and keeps what the demo player shows:
when each Card ran (the order of the replay) and each Card's last display.

Nothing here is written by hand: every value comes from RunInvocationsRequest
and RunRecordRequest of the run named in the file's `run` note.

  nix develop --command python3 docs/site/player/harvest.py report collatz ...
  TSUGUMI_BUILD_DIR=<build dir>  (default: <repo>/build; core/tsugumid, sdk/tsugumi-worker)
  TSUGUMI_PY_BINDINGS=<dir>      (default: <build dir>/generated/python)
  --no-sandbox                   run the Cards without Seatbelt, as tests/python does on a host
  --ports                        also write assets/ports.json: the core's resolved ports of every project
Writes docs/site/player-data/assets/<demo>.run.json and its images next to it.
`card-guide` is the project of docs/CardGuide.md instead of a folder in demos/.
"""

from __future__ import annotations

import base64
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from array import array
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build"))
os.environ["TSUGUMI_BUILD_DIR"] = str(BUILD)
os.environ.setdefault("TSUGUMI_PY_BINDINGS", str(BUILD / "generated" / "python"))
sys.path.insert(0, str(ROOT / "tests" / "python"))

from conftest import Daemon  # noqa: E402
from projects import ENDED, start_run, wait_run  # noqa: E402
from tsugumi_client import RpcFailure, path, text  # noqa: E402
from Tsugumi.Data.CardState import CardState  # noqa: E402
from Tsugumi.Data.Phase import Phase  # noqa: E402
from Tsugumi.Data.RunState import RunState  # noqa: E402
from Tsugumi.Data.StageOutcome import StageOutcome  # noqa: E402

OUT = ROOT / "docs" / "site" / "player-data" / "assets"
STATE = {v: k for k, v in vars(CardState).items() if not k.startswith("_")}
PHASE = {v: k for k, v in vars(Phase).items() if not k.startswith("_")}
OUTCOME = {v: k for k, v in vars(StageOutcome).items() if not k.startswith("_")}
RUN = {v: k for k, v in vars(RunState).items() if not k.startswith("_")}
MAX_IMAGE = 640      # pixels on the long side
MAX_PNG = 220_000    # bytes of a PNG before it becomes a JPEG
MAX_TEXT = 4000      # characters of a text display
MAX_ROWS = 24        # rows of a table
MAX_POINTS = 400     # points of a series


def guide_files() -> dict[str, str]:
    """The Python Card of docs/CardGuide.md, parsed as tests/python/test_sdk_examples.py does."""
    page = (ROOT / "docs" / "CardGuide.md").read_text()
    files: dict[str, dict[str, str]] = {}
    for m in re.finditer(r"^`([^`\n]+)`(?:（([^）\n]+)）)?\n\n```\w*\n(.*?)^```", page, flags=re.M | re.S):
        files.setdefault(m.group(2) or "", {})[m.group(1)] = m.group(3)
    return files[""]


def project_copy(demo: str, tmp: Path, sandbox: bool) -> Path:
    dst = tmp / demo
    if demo == "card-guide":
        for name, content in guide_files().items():
            p = dst / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        (dst / "layout.json").write_text("{}")
    else:
        shutil.copytree(ROOT / "demos" / "en" / demo, dst)
    if not sandbox:
        for f in dst.rglob("definitions/*/_card.json"):
            card = json.loads(f.read_text())
            if card.get("language"):
                card["sandbox"] = False
                f.write_text(json.dumps(card))
    return dst


def card_paths(root: Path, prefix: list[str] | None = None) -> list[list[str]]:
    """Every Card of a project, also inside its Cards in Card."""
    prefix = prefix or []
    doc = json.loads((root / "tsugumi.json").read_text())
    out: list[list[str]] = []
    for c in doc["canvas"]["cards"]:
        cid = c["id"]
        out.append([*prefix, cid])
        link = root / "definitions" / cid / "_link.json"
        if link.exists():
            ext = json.loads(link.read_text())["external"]
            out += card_paths(root / "externals" / ext, [*prefix, cid])
    return out


def resized(data: bytes, media: str, dest: Path) -> str:
    """Writes the image at a size for the page with sips (macOS); its file name."""
    suffix = ".jpg" if "jpeg" in media else ".png"
    dest = dest.with_suffix(suffix)
    dest.write_bytes(data)
    subprocess.run(["sips", "-Z", str(MAX_IMAGE), str(dest)], check=True, capture_output=True)
    if dest.stat().st_size > MAX_PNG:  # a photograph-like image: JPEG keeps it small
        jpeg = dest.with_suffix(".jpg")
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "62", str(dest), "--out", str(jpeg)], check=True, capture_output=True)
        dest.unlink()
        return jpeg.name
    return dest.name


def plain(values) -> list:
    """A Plotly array: a list, or numpy's {"dtype", "bdata"} from fig.to_json()."""
    if isinstance(values, dict) and "bdata" in values:
        kinds = {"f8": "d", "f4": "f", "i4": "i", "i2": "h", "i1": "b", "u4": "I", "u2": "H", "u1": "B"}
        return array(kinds[values["dtype"]], base64.b64decode(values["bdata"])).tolist()
    return list(values)


def thin(values, most: int) -> list[float]:
    values = plain(values)
    if len(values) <= most:
        return [round(float(v), 4) for v in values]
    step = (len(values) - 1) / (most - 1)
    return [round(float(values[round(i * step)]), 4) for i in range(most)]


def vega_image(spec: dict, dest: Path) -> str:
    """Writes the figure as an image, drawn by Vega (PNG: the Mac's thumbnails read images with ImageIO, which has no SVG); its file name."""
    drawn = subprocess.run(["node", str(Path(__file__).with_name("vega-svg.mjs"))], input=json.dumps(spec),
                           check=True, capture_output=True, text=True)
    with tempfile.TemporaryDirectory(prefix="tsg-harvest-") as tmp:
        svg, png = Path(tmp) / "figure.svg", Path(tmp) / "figure.png"
        svg.write_text(drawn.stdout)
        subprocess.run(["sips", "-s", "format", "png", str(svg), "--out", str(png)], check=True, capture_output=True)
        return resized(png.read_bytes(), "image/png", dest)


def module_value(name: str, data: dict, dest: Path) -> dict:
    """What can be drawn without the plot libraries: the figure's own points."""
    series = []
    if name == "plotly":
        for t in data.get("data", []):
            if "x" in t and "y" in t:
                series.append({"label": t.get("name") or "", "x": thin(t["x"], MAX_POINTS), "y": thin(t["y"], MAX_POINTS),
                               "points": "markers" in str(t.get("mode", ""))})
        layout = data.get("layout", {})
        title = (layout.get("title") or {}).get("text", "")
        axis = lambda a: ((layout.get(a) or {}).get("title") or {}).get("text", "")  # noqa: E731
        return {"type": "plot", "library": "Plotly", "title": title, "xLabel": axis("xaxis"), "yLabel": axis("yaxis"), "series": series}
    if name == "vega-lite":
        rows: list[dict] = []
        if isinstance(data.get("datasets"), dict):
            for v in data["datasets"].values():
                rows += v
        rows += (data.get("data") or {}).get("values", []) if isinstance(data.get("data"), dict) else []
        if isinstance(data.get("layer"), list):
            for layer in data["layer"]:
                rows += ((layer.get("data") or {}).get("values", [])) if isinstance(layer.get("data"), dict) else []
        enc = data.get("encoding") or (data.get("layer") or [{}])[0].get("encoding", {})
        xf = (enc.get("x") or {}).get("field")
        yf = (enc.get("y") or {}).get("field")
        cf = (enc.get("color") or {}).get("field")
        groups: dict[str, list[dict]] = {}
        for r in rows:
            groups.setdefault(str(r.get(cf, "")) if cf else "", []).append(r)
        for label, items in groups.items():
            # layers can hold rows of other columns (a band's low and high, say)
            items = [r for r in items if xf in r and yf in r] if xf and yf else []
            if items:
                series.append({"label": label, "x": thin([r[xf] for r in items], MAX_POINTS), "y": thin([r[yf] for r in items], MAX_POINTS), "points": False})
        title = data.get("title", "")
        if isinstance(title, dict):
            title = title.get("text", "")
        if not series:  # panels with their own axes (concat, facet): the picture, not one set of points
            return {"type": "image", "src": f"assets/{vega_image(data, dest)}", "alt": title}
        return {"type": "plot", "library": "Altair", "title": title, "xLabel": xf or "", "yLabel": yf or "", "series": series}
    return {"type": "module", "name": name}


def display_value(d, demo: str, card: list[str]) -> dict | None:
    v = d.value
    t = d.valueType
    if t == 1:
        s = text(v.text)
        return {"type": "text", "text": s[:MAX_TEXT], "markdown": bool(v.markdown), **({"cut": len(s)} if len(s) > MAX_TEXT else {})}
    if t == 2:
        return {"type": "scalar", "value": v.value, "label": text(v.label)}
    if t == 3:
        return {"type": "plot", "library": "line", "title": "", "xLabel": "", "yLabel": "",
                "series": [{"label": text(s.label), "x": thin(s.x, MAX_POINTS), "y": thin(s.y, MAX_POINTS), "points": False} for s in v.series]}
    if t == 4:
        rows = [[text(c) for c in r.cells] for r in v.rows]
        return {"type": "table", "columns": [text(c) for c in v.columns], "rows": rows[:MAX_ROWS], "totalRows": len(rows)}
    if t == 5:
        name = resized(bytes(v.data), text(v.mediaType), OUT / f"{demo}-{'-'.join(card)}")
        return {"type": "image", "src": f"assets/{name}", "alt": text(v.alt) if v.alt else ""}
    if t == 6:
        return {"type": "points", "count": len(v.x)}
    if t == 7:
        return module_value(text(v.name), json.loads(text(v.dataJson)), OUT / f"{demo}-{'-'.join(card)}")
    return None


def invocations(c, run_id: str, card: list[str]) -> list:
    out, cursor = [], None
    while True:
        extra = {"cursor": cursor} if cursor else {}
        page = c.call("RunInvocationsRequest", runId=run_id, card=path(*card), limit=500, **extra)
        out += page.records
        cursor = text(page.nextCursor) if page.nextCursor else None
        if not cursor:
            return out


def harvest(demo: str, tmp: Path, sandbox: bool, timeout: float) -> dict:
    root = project_copy(demo, tmp, sandbox)
    daemon = Daemon(root).start()
    began = time.time()
    try:
        c = daemon.client()
        c.hello()
        run_id = start_run(c, ["start"], concurrency=4)
        status = wait_run(c, run_id, timeout)
        state = RUN.get(status.state, str(status.state))
        cards = {}
        t0 = None
        outputs = {}
        artifacts = []
        for card in card_paths(root):
            k = "/".join(card)
            try:
                recs = invocations(c, run_id, card)
            except RpcFailure:
                recs = []
            if not recs:
                continue
            for r in recs:
                if r.startedAt:
                    t0 = r.startedAt if t0 is None else min(t0, r.startedAt)
            cards[k] = recs
            rec = c.call("RunRecordRequest", runId=run_id, card=path(*card)).record
            if len(card) == 1 and rec and rec.display and rec.display.value is not None:
                value = display_value(rec.display, demo, card)
                if value:
                    outputs[k] = value
        timeline = {}
        for k, recs in cards.items():
            first, last = recs[0], recs[-1]
            timeline[k] = {
                "count": len(recs), "lastState": STATE.get(last.state, str(last.state)),
                "startMs": int(first.startedAt - t0) if first.startedAt and t0 else None,
                "endMs": int(last.endedAt - t0) if last.endedAt and t0 else None,
                "stages": [{"phase": PHASE.get(s.phase, ""), "outcome": OUTCOME.get(s.outcome, ""),
                            "ms": int(s.end - s.start) if s.start and s.end else None} for s in (first.stages or [])],
                # What this Card sent in the whole run (a loop's branches differ per invocation).
                "sent": sorted({text(p) for r in recs for p in (r.outputsSent or [])}),
            }
        for a in c.call("ArtifactListRequest", runId=run_id, limit=50).artifacts:
            artifacts.append({"card": "/".join(text(s) for s in a.card.segments), "output": text(a.output), "name": text(a.name),
                              "members": [{"path": text(m.path), "size": int(m.sizeBytes or 0)} for m in a.members]})
        failure = text(status.failure.diagnostic) if getattr(status, "failure", None) and status.failure.diagnostic else None
        return {
            "run": {
                "id": run_id, "state": state, "date": date.today().isoformat(), "wallSeconds": round(time.time() - began, 1),
                "host": f"{platform.system()} {platform.machine()}, tsugumid on the host",
                "sandbox": "Seatbelt" if sandbox else "off", "commit": subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip(),
                "failure": failure,
            },
            "timeline": timeline, "outputs": outputs, "artifacts": artifacts,
        }
    finally:
        daemon.cleanup()


def resolved_ports() -> dict:
    """The core's resolved ports of every listed demo and of the guide's project (nothing runs)."""
    names = [d["id"] for d in json.loads((ROOT / "demos" / "en" / "index.json").read_text())["demos"]] + ["card-guide"]
    out: dict[str, dict] = {}
    for demo in names:
        with tempfile.TemporaryDirectory(prefix="tsg-harvest-") as tmp:
            daemon = Daemon(project_copy(demo, Path(tmp), True)).start()
            try:
                c = daemon.client()
                c.hello()
                port = lambda p: {"name": text(p.name), "rootType": text(p.rootType) if p.rootType else "", "optional": bool(p.optional)}  # noqa: E731
                out[demo] = {"/".join(text(s) for s in r.card.segments): {"inputs": [port(p) for p in r.inputs], "outputs": [port(p) for p in r.outputs]}
                             for r in c.call("ProjectInspectRequest").resolution}
            finally:
                daemon.cleanup()
        print(f"{demo}: {len(out[demo])} Cards", flush=True)
    return out


def main() -> int:
    args = sys.argv[1:]
    sandbox = "--no-sandbox" not in args
    demos = [a for a in args if not a.startswith("--")]
    OUT.mkdir(parents=True, exist_ok=True)
    if "--ports" in args:
        (OUT / "ports.json").write_text(json.dumps(resolved_ports(), ensure_ascii=False, separators=(",", ":")))
    code = 0
    for demo in demos:
        print(f"== {demo}", flush=True)
        with tempfile.TemporaryDirectory(prefix="tsg-harvest-") as tmp:
            try:
                result = harvest(demo, Path(tmp), sandbox, float(os.environ.get("HARVEST_TIMEOUT", "600")))
            except Exception as e:  # one demo failing does not stop the others
                print(f"{demo}: FAILED {type(e).__name__}: {e}", flush=True)
                code = 1
                continue
        if result["run"]["state"] == "succeeded":  # a run that failed is no record to replay
            (OUT / f"{demo}.run.json").write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
        print(f"{demo}: {result['run']['state']} in {result['run']['wallSeconds']} s, "
              f"{len(result['timeline'])} Cards, displays: {sorted(result['outputs'])}, failure: {result['run']['failure']}", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
