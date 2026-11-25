"""Runs demos for real with a host tsugumid and keeps what the demo player shows:
when each Card ran (the order of the replay) and the artifacts the run kept.
What a Card sends to its views is live and not recorded, so a recording holds
no display; a figure for the page is an artifact or an authored asset.

Nothing here is written by hand: every value comes from RunInvocationsRequest
and RunRecordRequest of the run named in the file's `run` note.

  nix develop --command python3 docs/site/player/harvest.py report collatz ...
  TSUGUMI_BUILD_DIR=<build dir>  (default: <repo>/build; tsugumid/tsugumid, workerd/tsugumi-worker)
  TSUGUMI_PY_BINDINGS=<dir>      (default: <build dir>/generated/python)
  --no-sandbox                   run the Cards without Seatbelt, as tests/python does on a host
  --ports                        also write assets/ports.json: the core's resolved ports of every project
Writes docs/site/player-data/assets/<demo>.run.json.
`card-guide` is the project of docs/CardGuide.md instead of a bundled demo.
"""

from __future__ import annotations

import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import time
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build"))
os.environ["TSUGUMI_BUILD_DIR"] = str(BUILD)
os.environ.setdefault("TSUGUMI_PY_BINDINGS", str(BUILD / "generated" / "python"))
sys.path.insert(0, str(ROOT / "tests" / "core"))

from harness.daemon import Daemon  # noqa: E402
from harness.demos import copy_demo, demos_dir  # noqa: E402
from harness.observations import read_pages  # noqa: E402
from projects import start_run, wait_run  # noqa: E402
from tsugumi_client import RpcFailure, path, text  # noqa: E402
from Tsugumi.Data.CardState import CardState  # noqa: E402
from Tsugumi.Data.Phase import Phase  # noqa: E402
from Tsugumi.Data.RunState import RunState  # noqa: E402
from Tsugumi.Data.StageOutcome import StageOutcome  # noqa: E402
from Tsugumi.RPC.NotificationPayload import NotificationPayload  # noqa: E402
from Tsugumi.Trace.Client import Client  # noqa: E402

OUT = ROOT / "docs" / "site" / "player-data" / "assets"
STATE = {v: k for k, v in vars(CardState).items() if not k.startswith("_")}
PHASE = {v: k for k, v in vars(Phase).items() if not k.startswith("_")}
OUTCOME = {v: k for k, v in vars(StageOutcome).items() if not k.startswith("_")}
RUN = {v: k for k, v in vars(RunState).items() if not k.startswith("_")}


# Preview selected retained results. View messages are never read into a recording.
PREVIEWS = {
    "report": {"render": "report.md"},
    "casa": {"rgb": "casa-rgb.png", "spectrum": "casa-spectrum.png"},
    "gw150914": {"chirp": "chirp.png"},
    "max-flow": {
        "flow": "flow.png",
        "cut": "cut.png",
        "sweep": "sweep.png",
        "verify": "summary.md",
        "whatif": "summary.md",
    },
}


def artifact_preview(client, artifact_id, member, demo, card, staging):
    content = read_pages(client, "ArtifactReadRequest", artifactId=artifact_id,
                         member=member, page_bytes=65536)
    if member.endswith(".md"):
        return {"type": "text", "markdown": True, "text": content.decode("utf-8")}
    filename = f"{demo}-{card}.png"
    (staging / filename).write_bytes(content)
    return {"type": "image", "src": f"assets/{filename}", "alt": f"{demo} · {card}"}


def guide_files() -> dict[str, str]:
    """The project files in named code blocks in docs/CardGuide.md."""
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
    else:
        copy_demo("en", demo, dst)
    if not sandbox:
        for f in dst.rglob("definitions/*/_card.json"):
            card = json.loads(f.read_text())
            if card.get("type") == "code":
                card["sandbox"].update(filesystem=False, pid=False, ipc=False, identity=False)
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
        definition = root / "definitions" / cid / "_card.json"
        if json.loads(definition.read_text())["type"] == "subgraph":
            out += card_paths(root / "externals" / cid, [*prefix, cid])
    return out


def invocations(c, run_id: str, card: list[str]) -> list:
    out, cursor = [], None
    while True:
        extra = {"cursor": cursor} if cursor else {}
        page = c.call("RunInvocationsRequest", runId=run_id, card=path(*card), limit=500, **extra)
        out += page.records
        cursor = text(page.nextCursor) if page.nextCursor else None
        if not cursor:
            return out


def send_seed(c, subscription: str, timeout: float) -> None:
    """Choose the Live Stream starting value as a click on the seed view would."""
    opened = c.wait_notification(
        lambda event: event.valueType == NotificationPayload.ViewOpened
        and text(event.value.name) == "seed",
        timeout=timeout,
    ).value
    session = text(opened.session)
    ready = c.wait_notification(
        lambda event: event.valueType == NotificationPayload.DisplayUpdated
        and text(event.value.display.session) == session
        and bytes(event.value.display.bytes) == b"\x01",
        timeout=timeout,
    ).value.display
    c.call("ViewCreditRequest", subscriptionId=subscription, session=session,
           bytes=1, creditId=ready.creditId)
    c.call("ViewSendRequest", session=session, to="settings", bytes=b"\x0a")


def harvest(demo: str, tmp: Path, staging: Path, sandbox: bool, timeout: float) -> dict:
    root = project_copy(demo, tmp, sandbox)
    graph_id = subprocess.check_output(
        ["node", str(Path(__file__).with_name("build.mjs")), "--graph-id", str(root)], text=True,
    ).strip()
    daemon = Daemon(root).start()
    began = time.time()
    try:
        c = daemon.connect(client=Client.Web) if demo == "live-stream" else daemon.connect()
        snapshot = c.call("ProjectInspectRequest")
        trigger = next(text(d.id) for d in snapshot.definitions
                       if json.loads(text(d.cardJson))["type"] == "trigger")
        if demo == "live-stream":
            subscription = text(c.open().subscriptionId)
        run_id = start_run(c, [trigger], concurrency=4)
        if demo == "live-stream":
            send_seed(c, subscription, timeout)
        status = wait_run(c, run_id, timeout=timeout)
        if status.state != RunState.succeeded:
            state = RUN.get(status.state, str(status.state))
            failure = text(status.failure.diagnostic) if status.failure else None
            raise RuntimeError(f"run {run_id} {state}: {failure}")
        cards = {}
        t0 = None
        artifacts = []
        for card in card_paths(root):
            k = "/".join(card)
            try:
                recs = invocations(c, run_id, card)
            except RpcFailure as error:
                if error.kind == "not_found" and error.args_map.get("card") == k:
                    continue
                raise
            if not recs:
                continue
            for r in recs:
                if r.startedAt:
                    t0 = r.startedAt if t0 is None else min(t0, r.startedAt)
            cards[k] = recs
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
        outputs = {}
        previews = PREVIEWS.get(demo, {})
        for a in c.call("ArtifactListRequest", runId=run_id, limit=50).artifacts:
            card = "/".join(text(segment) for segment in a.card.segments)
            artifact_id = text(a.artifactId)
            members = [{"path": text(m.path), "size": int(m.sizeBytes or 0)} for m in a.members]
            artifacts.append({"card": card, "output": artifact_id, "name": artifact_id, "members": members})
            preview = previews.get(card)
            if preview and any(member["path"] == preview for member in members):
                outputs[card] = artifact_preview(c, artifact_id, preview, demo, card, staging)
        return {
            "graphId": graph_id,
            "run": {
                "id": run_id, "state": "succeeded", "date": date.today().isoformat(), "wallSeconds": round(time.time() - began, 1),
                "host": f"{platform.system()} {platform.machine()}, tsugumid on the host",
                "sandbox": "Seatbelt" if sandbox else "sandbox options off", "commit": subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True).strip(),
            },
            "timeline": timeline, "outputs": outputs, "artifacts": artifacts,
        }
    finally:
        daemon.close()


def resolved_ports() -> dict:
    """The core's resolved ports of every listed demo and of the guide's project (nothing runs)."""
    names = [d["id"] for d in json.loads((demos_dir() / "en" / "index.json").read_text())["demos"]] + ["card-guide"]
    out: dict[str, dict] = {}
    for demo in names:
        with tempfile.TemporaryDirectory(prefix="tsg-harvest-") as tmp:
            daemon = Daemon(project_copy(demo, Path(tmp), True)).start()
            try:
                c = daemon.connect()

                def port(p):
                    members = [text(name) for name in p.mergeMemberTypes or []]
                    type_name = " | ".join(members) or text(p.rootType or b"")
                    return {"name": text(p.name), "rootType": type_name}
                out[demo] = {"/".join(text(s) for s in r.card.segments): {"inputs": [port(p) for p in r.inputs], "outputs": [port(p) for p in r.outputs]}
                             for r in c.call("ProjectInspectRequest").resolution}
            finally:
                daemon.close()
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
        with (
            tempfile.TemporaryDirectory(prefix="tsg-harvest-") as tmp,
            tempfile.TemporaryDirectory(prefix=".harvest-", dir=OUT.parent) as staging_dir,
        ):
            try:
                staging = Path(staging_dir)
                result = harvest(demo, Path(tmp), staging, sandbox, float(os.environ.get("HARVEST_TIMEOUT", "600")))
                record = staging / f"{demo}.run.json"
                record.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
                for asset in staging.glob("*.png"):
                    asset.replace(OUT / asset.name)
                record.replace(OUT / record.name)
            except Exception as e:  # one demo failing does not stop the others
                print(f"{demo}: FAILED {type(e).__name__}: {e}", flush=True)
                code = 1
                continue
        print(f"{demo}: succeeded in {result['run']['wallSeconds']} s, "
              f"{len(result['timeline'])} Cards, {len(result['artifacts'])} artifacts", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
