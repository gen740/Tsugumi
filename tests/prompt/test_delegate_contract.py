"""Offline regressions for delegate instructions; no model or socket is needed."""
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class DelegateContractTests(unittest.TestCase):
    def test_prompt_example_uses_only_supported_task_arguments(self):
        prompt = (ROOT / "agent/Shared/system-prompt.md").read_text()
        example = next(line.strip().removeprefix("delegate ") for line in prompt.splitlines()
                       if line.strip().startswith("delegate {"))
        arguments = json.JSONDecoder().raw_decode(example)[0]
        schema = json.loads((ROOT / "tests/fixtures/agent-contract.json").read_text())["delegateSchema"]
        self.assertEqual(set(arguments), set(schema["required"]))
        task_schema = schema["properties"]["tasks"]["items"]
        for task in arguments["tasks"]:
            # The example shows the scope sets, which are optional in the schema
            # (omitting both is the unscoped form a person has to enable).
            self.assertLessEqual(set(task_schema["required"]), set(task))
            self.assertLessEqual({"read_cards", "write_cards"}, set(task))
            self.assertLessEqual(set(task), set(task_schema["properties"]))

    def test_delegate_texts_do_not_promise_legacy_host_behaviors(self):
        shared = json.loads((ROOT / "agent/Shared/agent.json").read_text())
        prompt = (ROOT / "agent/Shared/system-prompt.md").read_text()
        docs = (ROOT / "docs/features.md").read_text()
        # These were promises of routing, automatic reads/retries or write enforcement.
        for text in (prompt, json.dumps(shared["delegateTool"]), docs):
            for stale in ('"runs"', '`runs`', '"together"', '"each"',
                          "never tsugumi.json", "app waits and sends it again",
                          "app sends it again", "read at the start, its Cards",
                          "delegate needs 2 to 8", "takes 2 to 8 tasks"):
                with self.subTest(stale=stale):
                    self.assertNotIn(stale, text)
        # A helper's read and write sets are enforced by the core now.
        for stale in ("Helpers may read the whole project", "not an enforced write boundary"):
            with self.subTest(stale=stale):
                self.assertNotIn(stale, prompt)
        self.assertIn('"read_cards"', prompt)
        self.assertIn('"write_cards"', prompt)
        self.assertIn("The core does not retry these failures for helpers", prompt)


if __name__ == "__main__":
    unittest.main()
