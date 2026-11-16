"""Measured writing and recovery instructions survive generation and ports."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CardPromptContractTests(unittest.TestCase):
    def test_object_api_and_recovery_are_explicit(self):
        # The always-sent rules plus the Card reference read on demand (demo_read reference/cards.md).
        prompt = " ".join(((ROOT / 'agent/Shared/system-prompt.md').read_text()
                           + (ROOT / 'demos/reference/cards.md').read_text()).split())
        for text in ('"inputs": [{"name": "config"}]', 'lowerCamelCase',
                     'inputs.events.value.decode()',
                     'return {"images": {"value": run.output_directory}}',
                     'A refused save, a validation error and a failed Card each name what to fix',
                     'Your turn ends at the first reply without a tool call',
                     "demo_list lists each demo's path, name and one line",
                     'wait_seconds (up to 300)', 'still_running'):
            with self.subTest(text=text):
                self.assertIn(text, prompt)
        self.assertNotIn('inputs.input("events", EventsFile)', prompt)


if __name__ == '__main__':
    unittest.main()
