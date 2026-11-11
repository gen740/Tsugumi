"""Opt-in live citation probe; manually check the reply against the recorded run.

Expected casa: counts 101187/157471/110852, spectrum succeeded, links to both
Cards and exact run records; no saves/runs, invented ids or unsupported claims.
Expected heldout: green 157471, linked bands and its run. Control: project name only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from inspect_probe import ROOT, run_trial, summarize
from evidence_fixture import RECORDING, SNAPSHOT, TOOLS

CASES = {
    'control': 'このプロジェクトの名前だけを教えてください。実行や変更は不要です。',
    'casa': 'Cas A の直近の実行について、bands の光子数と spectrum の完了状態を短く説明してください。再実行や変更はしないでください。',
    'heldout': '今回の Cas A ではどの色の帯域に光子が最も多く含まれていますか。使った Card と実行もわかるよう、短く教えてください。',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', choices=CASES)
    parser.add_argument('--prompt', type=Path, default=ROOT / 'agent/Shared/system-prompt.md')
    parser.add_argument('--model', default='gpt-6.1-sol')
    parser.add_argument('--codex', default=os.environ.get('CODEX_BIN', 'codex'))
    parser.add_argument('--events', type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).with_name('evidence_fixture.py').resolve()
    approved = ('project_inspect', 'run_record', 'run_status', 'run_history')
    stdout, error, version, prompt_hash = run_trial(CASES[args.case], args.prompt, fixture, args.model, args.codex, approved)
    if args.events:
        args.events.write_text(stdout)
    report = summarize(args.case, stdout, error)
    for call in report['calls']:
        call.pop('snapshot_received', None)  # The inspection probe's snapshot is a different project.
    if any(call.get('error') or call.get('status') == 'failed' for call in report['calls']):
        report.update(outcome='invalid', reason='Tool replay failed; exclude this trial')
    data = fixture.read_bytes() + json.dumps([TOOLS, SNAPSHOT, RECORDING], sort_keys=True).encode()
    print(json.dumps({'case': args.case, 'model': args.model, 'effort': 'low', 'codex_version': version,
                      'instructions_sha256': prompt_hash, 'fixture_sha256': hashlib.sha256(data).hexdigest(),
                      **report}, ensure_ascii=False, indent=2))
    return {'needs_review': 0, 'fail': 1, 'invalid': 2}[report['outcome']]


if __name__ == '__main__':
    sys.exit(main())
