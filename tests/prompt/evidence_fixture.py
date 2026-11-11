"""Read-only MCP replay of the recorded Cas A demo for citation probes.

The run id, states, counts and displays come from the checked-in recording.
RPC envelopes are reconstructed; this is not a live core or a new science run.
"""
import json
import sys
from pathlib import Path
from inspect_fixture import TOOLS, reply as common_reply

ROOT = Path(__file__).resolve().parents[2]
RECORDING = json.loads((ROOT / 'docs/site/player-data/assets/casa.run.json').read_text())
PROJECT = ROOT / 'demos/en/casa'
RUN = RECORDING['run']['id']
STATUS = {'run_id': RUN, 'state': 'succeeded'}
SNAPSHOT = {'project_id': 'd509c04f-8972-4c49-afec-2aba11cf71cf', 'rev': '0',
            'project_json': (PROJECT / 'tsugumi.json').read_text(), 'run': STATUS,
            'definitions': [{'id': p.parent.name, 'card_json': p.read_text(),
                             'meta_json': p.with_name('_meta.json').read_text()}
                            for p in sorted(PROJECT.glob('definitions/*/_card.json'))]}


def reply(request):
    if request['method'] != 'tools/call':
        return common_reply(request)
    name = request['params']['name']
    args = request['params'].get('arguments', {})
    if name == 'project_inspect' and not args:
        kind, value = 'ProjectSnapshot', SNAPSHOT
    elif name == 'run_status' and args.get('run_id', RUN) == RUN:
        kind, value = 'RunStatus', STATUS
    elif name == 'run_history':
        kind, value = 'RunPage', {'runs': [{'status': STATUS}]}
    elif name == 'run_record' and args.get('run_id') == RUN:
        parts = args.get('card', {}).get('segments', [])
        card = '/'.join(parts)
        if card not in RECORDING['timeline'] or args.get('invocation', '1') != '1':
            return {'isError': True, 'content': [{'type': 'text', 'text': 'Record not found'}]}
        summary = {'card': {'segments': parts}, 'invocation': '1', 'state': 'succeeded'}
        output = RECORDING['outputs'].get(card)
        display = None
        if output:
            display = {'card': {'segments': parts}, 'run_id': RUN, 'invocation': '1', 'revision': '1'}
            if output['type'] == 'table':
                display.update(value_type='DisplayTable', value={'columns': output['columns'], 'rows': [{'cells': row} for row in output['rows']]})
            else:
                # The probe checks completion, not interpretation of image pixels.
                display.update(value_type='DisplayImage', value={'media_type': 'image/png', 'data': [], 'alt': output['alt']})
        kind, value = 'RecordResult', {'selected_invocation': '1', 'record': {'summary': summary, 'display': display, 'inputs': [], 'outputs': [], 'input_sources': []},
                                      'run': {'status': STATUS}, 'status': {'state': 'succeeded'}, 'exists': True}
    else:
        return {'isError': True, 'content': [{'type': 'text', 'text': 'This read-only replay does not support that request.'}]}
    return {'isError': False, 'content': [{'type': 'text', 'text': json.dumps({'reply': kind, 'value': value})}]}


if __name__ == '__main__':
    for line in sys.stdin:
        request = json.loads(line)
        if 'id' not in request:
            continue
        response = {'jsonrpc': '2.0', 'id': request['id']}
        try:
            response['result'] = reply(request)
        except KeyError:
            response['error'] = {'code': -32601, 'message': 'Unknown request'}
        print(json.dumps(response), flush=True)
