"""Measured writing and recovery instructions survive generation and ports."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CardPromptContractTests(unittest.TestCase):
    def test_object_api_and_recovery_are_explicit(self):
        prompt = (ROOT / 'agent/Shared/system-prompt.md').read_text()
        for text in ('"inputs": [{"name": "config"}]', 'lowerCamelCase',
                     'inputs.events.value.decode()',
                     'return {"images": {"value": run.output_directory}}',
                     'When a save is refused, project_validate reports an error',
                     'make that call now', 'call it only when no path named here fits',
                     'wait_seconds: 300', 'do not poll'):
            with self.subTest(text=text):
                self.assertIn(text, prompt)
        self.assertNotIn('inputs.input("events", EventsFile)', prompt)


if __name__ == '__main__':
    unittest.main()
