"""Measures the evaluation metrics in the real container environment.

    nix develop -c python tests/python/measure.py [--fresh]

--fresh deletes the demo store volume first, so "first preparation"
includes downloading the pinned toolchains into the persistent store.
Prints a JSON report.
"""

from __future__ import annotations

import argparse
import json
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from conftest import ROOT, ContainerEnv  # noqa: E402
from test_demos import connect, run, wait_run  # noqa: E402
from tsugumi_client import path, text  # noqa: E402
from Tsugumi.Data.RunState import RunState  # noqa: E402
from Tsugumi.RPC.NotificationPayload import NotificationPayload  # noqa: E402


def sh(*args) -> str:
    return subprocess.run(args, capture_output=True, text=True).stdout.strip()


def in_env(env: ContainerEnv, script: str) -> str:
    return sh("container", "exec", env.name, "/bin/bash", "-c", script)


def du(env: ContainerEnv, p: str) -> int:
    out = in_env(env, f"du -sb {p} 2>/dev/null | cut -f1")
    return int(out) if out.isdigit() else 0


def rss_kib(env: ContainerEnv) -> int:
    out = in_env(env, "for p in /proc/[0-9]*; do [[ \"$(cat $p/comm 2>/dev/null)\" == tsugumid ]] && "
                      "{ while read k v u; do [[ $k == VmRSS: ]] && echo $v; done < $p/status; }; done")
    return int(out.splitlines()[0]) if out else 0


def workers(env: ContainerEnv) -> int:
    out = in_env(env, "n=0; for p in /proc/[0-9]*; do c=$(cat $p/comm 2>/dev/null); "
                      "[[ $c == tsugumi-worker || $c == bwrap ]] && n=$((n+1)); done; echo $n")
    return int(out or 0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fresh", action="store_true")
    a = ap.parse_args()
    report: dict = {
        "machine": {"model": sh("sysctl", "-n", "hw.model"), "chip": sh("sysctl", "-n", "machdep.cpu.brand_string"),
                    "memory_gib": int(sh("sysctl", "-n", "hw.memsize")) // (1 << 30),
                    "macos": platform.mac_ver()[0], "container": sh("container", "--version")},
        "environment": {"cpus": 2, "memory_gib": 4, "store_volume_gib": 20},
    }
    img = json.loads(sh("container", "image", "inspect", "tsugumi-env:1.0.0") or "[{}]")
    tar = ROOT / "dist" / "tsugumi-env.tar"
    report["image"] = {
        "oci_archive_bytes": tar.stat().st_size if tar.exists() else None,
        "inspect": {k: img[0].get(k) for k in ("size", "index") if isinstance(img, list) and img} if img else None,
    }
    tmp = Path(tempfile.mkdtemp())
    results = {}
    for demo in ("collatz", "report", "live-stream"):
        dst = tmp / demo
        shutil.copytree(ROOT / "demos" / demo, dst)
        env = ContainerEnv(dst, f"tsugumi-measure-{demo}")
        if a.fresh:
            subprocess.run(["container", "rm", "-f", env.name], capture_output=True)
            subprocess.run(["container", "volume", "rm", env.volume], capture_output=True)
        t0 = time.time()
        env.start()
        t_ready = time.time() - t0
        c = connect(env)
        t_open0 = time.time()
        c.open()
        t_open = time.time() - t_open0
        image_root = du(env, "/nix/store")  # lower (image) + upper (persistent) union
        t1 = time.time()
        rid = run(c, ["begin"])
        st = wait_run(c, rid, timeout=3600)
        first_run = time.time() - t1
        store_after = du(env, "/persist/nix/upper")
        # Cached: recreate the container and run again.
        c.close()
        env.stop()
        t2 = time.time()
        env.start()
        c = connect(env)
        c.open()
        cached_start = time.time() - t2
        t3 = time.time()
        rid2 = run(c, ["begin"])
        wait_run(c, rid2, timeout=3600)
        cached_run = time.time() - t3
        results[demo] = {
            "first_state": st.state, "env_ready_s": round(t_ready, 2), "open_s": round(t_open, 3),
            "first_run_s": round(first_run, 1), "cached_start_to_operable_s": round(cached_start, 2),
            "cached_run_s": round(cached_run, 2), "store_union_bytes": image_root, "store_persisted_bytes": store_after,
            "history_bytes": (dst / ".tsugumi" / "history.sqlite").stat().st_size,
            "runs_dir_bytes": sum(f.stat().st_size for f in (dst / ".tsugumi" / "runs").rglob("*") if f.is_file()),
        }
        if demo == "live-stream":
            results[demo].update(live_metrics(env, c))
        c.close()
        env.cleanup()
    report["demos"] = results
    print(json.dumps(report, indent=2))


def live_metrics(env: ContainerEnv, c) -> dict:
    out: dict = {}
    # RPC responsiveness while the stream runs (proxy for UI input latency).
    opened = c.open()
    c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("plot")])
    rid = run(c, ["begin"])
    lat = []
    other = connect(env)
    for _ in range(100):
        t = time.time()
        other.call("RunStatusRequest", runId=rid)
        lat.append((time.time() - t) * 1000)
        time.sleep(0.05)
    out["status_latency_ms_p50"] = round(statistics.median(lat), 2)
    out["status_latency_ms_p99"] = round(sorted(lat)[98], 2)
    t = time.time()
    c.call("RunCancelRequest", runId=rid)
    out["cancel_reply_ms"] = round((time.time() - t) * 1000, 1)
    wait_run(c, rid, timeout=30)
    out["cancel_to_cancelled_s"] = round(time.time() - t, 2)
    # Ten complete runs: memory, workers and storage must not accumulate.
    rss0, store0 = rss_kib(env), du(env, "/persist/nix/upper")
    hist0 = (env.project / ".tsugumi" / "history.sqlite").stat().st_size
    for _ in range(10):
        wait_run(c, run(c, ["begin"]), timeout=600)
    time.sleep(1)
    out["ten_runs"] = {
        "rss_kib_before": rss0, "rss_kib_after": rss_kib(env),
        "workers_after": workers(env),
        "store_growth_bytes": du(env, "/persist/nix/upper") - store0,
        "history_growth_bytes": (env.project / ".tsugumi" / "history.sqlite").stat().st_size - hist0,
    }
    other.close()
    return out


if __name__ == "__main__":
    main()
