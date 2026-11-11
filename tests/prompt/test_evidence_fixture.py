"""Ensure the citation replay preserves the recording and cannot mutate a project."""
import json
import unittest
from evidence_fixture import RUN, reply


def call(name, args):
    return reply({'method': 'tools/call', 'params': {'name': name, 'arguments': args}})


class EvidenceFixtureTests(unittest.TestCase):
    def test_record_preserves_casa_identity_and_counts(self):
        result = call('run_record', {'run_id': RUN, 'card': {'segments': ['bands']}, 'invocation': '1'})
        self.assertFalse(result['isError'])
        record = json.loads(result['content'][0]['text'])['value']['record']
        self.assertEqual(record['summary']['invocation'], '1')
        self.assertEqual(record['display']['run_id'], RUN)
        self.assertEqual([r['cells'][2] for r in record['display']['value']['rows']], ['101187', '157471', '110852'])

    def test_only_recorded_targets_and_reads_are_supported(self):
        for name, args in [
            ('run_record', {'run_id': 'other', 'card': {'segments': ['bands']}}),
            ('run_record', {'run_id': RUN, 'card': {'segments': ['missing']}}),
            ('run_record', {'run_id': RUN, 'card': {'segments': ['bands']}, 'invocation': '9'}),
            ('project_save', {}), ('run_start', {}),
        ]:
            self.assertTrue(call(name, args)['isError'])
