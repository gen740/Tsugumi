"""The real script launcher supplies background and script-only trust arguments."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("launch_result", ["done", "trust_error", "steps_only"])
def test_run_ui_passes_background_and_script_trust_arguments(tmp_path, launch_result):
    commands = tmp_path / 'commands'
    commands.mkdir()
    record = tmp_path / 'arguments'
    # No app is opened. The fake launcher completes the script immediately.
    (commands / 'open').write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
Path(os.environ['LAUNCH_RECORD']).write_text('\\n'.join(sys.argv[1:]))
for arg in sys.argv:
    if arg.startswith('TSUGUMI_UI_LOG='):
        result = os.environ['LAUNCH_RESULT']
        text = 'step wait 0\\n' if result == 'steps_only' else ('trust-error unavailable\\n' if result == 'trust_error' else '') + 'done\\n'
        Path(arg.split('=', 1)[1]).write_text(text)
''')
    (commands / 'pgrep').write_text('#!/bin/sh\necho 2147483647\n')
    (commands / 'sleep').write_text('#!/bin/sh\nexit 0\n')
    for command in commands.iterdir():
        command.chmod(0o755)
    script = tmp_path / 'trial.script'
    script.write_text('wait 0\n')
    output = tmp_path / 'output'
    env = {**os.environ, 'PATH': f'{commands}:{os.environ["PATH"]}', 'LAUNCH_RECORD': str(record),
           'TSUGUMI_UI_TRUSTED_FOLDER': str(tmp_path), 'TSUGUMI_HOST_STATE': str(tmp_path / 'host-core'), 'TSUGUMI_UI_SECONDS': '1', 'LAUNCH_RESULT': launch_result}
    result = subprocess.run(['bash', str(ROOT / 'macOS/tools/run-ui.sh'), str(tmp_path / 'Tsugumi.app'), str(script), str(output)],
                   env=env, capture_output=True, text=True, timeout=10)
    assert result.returncode == (0 if launch_result == "done" else 1)
    args = record.read_text().splitlines()
    assert '-g' in args and '-n' in args
    assert f'TSUGUMI_UI_SCRIPT={script}' in args
    assert f'TSUGUMI_UI_TRUSTED_FOLDER={tmp_path}' in args
    assert f'TSUGUMI_HOST_STATE={tmp_path / "host-core"}' in args
