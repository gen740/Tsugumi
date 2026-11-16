"""The deterministic model fixture selects guide behavior from capabilities."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('model_fixture', ROOT / 'tests/fixtures/model_fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class FixtureRoleTests(unittest.TestCase):
    def test_guide_role_works_with_arbitrary_prompt_text(self):
        messages = [{'role': 'user', 'content': 'Explain this project.'}]
        self.assertEqual(fixture.decide(messages, 'A different instruction.', ['project_inspect', 'guide_show']),
                         ('tool', 'project_inspect', {}))

    def test_main_role_does_not_get_guide_actions_from_its_prompt(self):
        messages = [{'role': 'user', 'content': 'Explain this project.'}]
        self.assertEqual(fixture.decide(messages, 'guide_show', ['project_inspect'])[0], 'text')


if __name__ == '__main__':
    unittest.main()
