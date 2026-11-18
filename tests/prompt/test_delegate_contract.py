"""Delegate schemas and executable example arguments; no model or socket is needed."""
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class DelegateContractTests(unittest.TestCase):
    def test_helpers_skill_example_uses_only_supported_task_arguments(self):
        prompt = (ROOT / "hostd/agent/Shared/helpers-skill.md").read_text()
        schema = json.loads((ROOT / "tests/fixtures/agent-contract.json").read_text())["delegateSchema"]
        for line in prompt.splitlines():
            if not line.strip().startswith("delegate {"):
                continue
            arguments = json.JSONDecoder().raw_decode(line.strip().removeprefix("delegate "))[0]
            self.assertLessEqual(set(schema["required"]), set(arguments))
            self.assertLessEqual(set(arguments), set(schema["properties"]))
            task_schema = schema["properties"]["tasks"]["items"]
            for task in arguments["tasks"]:
                self.assertLessEqual(set(task_schema["required"]), set(task))
                self.assertLessEqual(set(task), set(task_schema["properties"]))
