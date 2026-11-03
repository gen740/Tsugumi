"""Start / run / stop cycles of a real environment, checking each VM shut
down cleanly: no kernel oops in its boot log, `stop` returned, and the
container is gone after `rm`. Used to check the guest kernel
(macOS/Container/kernel.nix; TSUGUMI_KERNEL overrides, "" = system default).

    TSUGUMI_TEST_CONTAINER=1 python stress_shutdown.py [rounds]
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from conftest import ROOT, ContainerEnv
from tsugumi_client import new_id, path


def main(rounds: int) -> int:
    bad = 0
    for i in range(1, rounds + 1):
        tmp = Path(tempfile.mkdtemp(prefix="tsgs-"))
        shutil.copytree(ROOT / "demos" / "collatz", tmp / "p")
        env = ContainerEnv(tmp / "p", f"tsugumi-stress-{i}")
        env.volume = "tsugumi-test-collatz-store"  # Nix store already filled by the demo tests
        env.start()
        c = env.client()
        snap = c.call("ProjectInspectRequest")
        acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="stress", card=path("start"))
        for _ in range(3000):
            st = c.call("RunStatusRequest", runId=acc.run.runId)
            if st.state in (6, 7, 8):
                break
            time.sleep(0.2)
        c.close()
        t0 = time.time()
        stop = subprocess.run(["container", "stop", "--time", "20", env.name], capture_output=True, timeout=180)
        took = time.time() - t0
        boot = subprocess.run(["container", "logs", "--boot", env.name], capture_output=True, text=True).stdout
        oops = boot.count("Internal error: Oops")
        subprocess.run(["container", "rm", "-f", env.name], capture_output=True, timeout=120)
        left = env.name in subprocess.run(["container", "ls", "-a"], capture_output=True, text=True).stdout
        ok = stop.returncode == 0 and oops == 0 and not left
        bad += not ok
        print(f"round {i}: run state={st.state} stop rc={stop.returncode} {took:.1f}s oops={oops} left={left}", flush=True)
        shutil.rmtree(env.control, ignore_errors=True)
        shutil.rmtree(tmp, ignore_errors=True)
    return bad


if __name__ == "__main__":
    sys.exit(1 if main(int(sys.argv[1]) if len(sys.argv) > 1 else 5) else 0)
