"""Offline regressions for delegate instructions; no model or socket is needed."""
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class DelegateContractTests(unittest.TestCase):
    def test_helpers_skill_example_uses_only_supported_task_arguments(self):
        prompt = (ROOT / "agent/Shared/helpers-skill.md").read_text()
        example = next(line.strip().removeprefix("delegate ") for line in prompt.splitlines()
                       if line.strip().startswith("delegate {"))
        arguments = json.JSONDecoder().raw_decode(example)[0]
        schema = json.loads((ROOT / "tests/fixtures/agent-contract.json").read_text())["delegateSchema"]
        self.assertEqual(set(arguments), set(schema["required"]))
        task_schema = schema["properties"]["tasks"]["items"]
        for task in arguments["tasks"]:
            # The example shows the scope sets, which are optional in the schema
            # (omitting all three is the unscoped form a person has to enable).
            self.assertLessEqual(set(task_schema["required"]), set(task))
            self.assertLessEqual({"read_cards", "write_cards", "run_cards"}, set(task))
            self.assertLessEqual(set(task), set(task_schema["properties"]))

    def test_capability_contract_replaces_the_parallelism_setting(self):
        shared = json.loads((ROOT / "agent/Shared/agent.json").read_text())["delegateTool"]
        fixture = json.loads((ROOT / "tests/fixtures/agent-contract.json").read_text())["delegateSchema"]
        self.assertEqual(shared["inputSchema"], fixture)
        tasks = fixture["properties"]["tasks"]
        self.assertEqual(tasks["minItems"], 1)
        self.assertNotIn("maxItems", tasks)
        task = tasks["items"]
        self.assertEqual(set(task["required"]), {"name", "instruction"})
        for capability in ("depth", "count"):
            self.assertEqual(task["properties"][capability],
                             {"type": "integer", "minimum": 0})
        text = shared["description"]
        for contract in ("default 0/0", "at most count children over its lifetime",
                         "depth <= its own depth - 1", "within its Card sets",
                         "The main Agent is unlimited", "user permission setting"):
            self.assertIn(contract, text)
        for source in ("protocol/host.fbs", "core/src/host/settings.cc",
                       "agent/Shared/agent.json", "agent/Shared/system-prompt.md"):
            self.assertNotIn("helperParallelism", (ROOT / source).read_text())

    def test_unnamed_helpers_skill_owns_the_split_policy(self):
        helpers = (ROOT / "agent/Shared/helpers-skill.md").read_text()
        prompt = (ROOT / "agent/Shared/system-prompt.md").read_text()
        metadata = helpers.split("---", 2)[1]
        self.assertIn("name: helpers", metadata)
        self.assertIn("always: true", metadata)
        self.assertNotIn("scope:", metadata)
        policy = "Do not split work among sub-agents unless the person's own message asks for helpers or parallel work"
        self.assertNotIn(policy, helpers)
        shared = json.loads((ROOT / "agent/Shared/agent.json").read_text())
        tool_text = json.dumps([shared["delegateTool"], *shared["hostTools"],
                                json.loads((ROOT / "tests/fixtures/agent-tools.json").read_text())])
        for text in (prompt, tool_text):
            self.assertNotIn(policy, text)
            self.assertNotIn("helper or parallel work requested by the person", text)
            self.assertNotIn("person's own message asks for helpers or parallel work", text)
        for removed in ("delegate {", policy, "Before delegating", "Each helper receives",
                        "Do the\n  work yourself when it is small"):
            self.assertNotIn(removed, prompt)

    def test_delegate_texts_do_not_promise_legacy_host_behaviors(self):
        shared = json.loads((ROOT / "agent/Shared/agent.json").read_text())
        prompt = (ROOT / "agent/Shared/system-prompt.md").read_text()
        docs = (ROOT / "docs/features.md").read_text()
        helpers = (ROOT / "agent/Shared/helpers-skill.md").read_text()
        # These were promises of routing, automatic reads/retries or write enforcement.
        for text in (prompt, helpers, json.dumps(shared["delegateTool"]), docs):
            for stale in ('"runs"', '`runs`', '"together"', '"each"',
                          "never tsugumi.json", "app waits and sends it again",
                          "app sends it again", "read at the start, its Cards",
                          "delegate needs 2 to 8", "takes 2 to 8 tasks"):
                with self.subTest(stale=stale):
                    self.assertNotIn(stale, text)
        # Exact helper sets are enforced by the host.
        for stale in ("Helpers may read the whole project", "not an enforced write boundary"):
            with self.subTest(stale=stale):
                self.assertNotIn(stale, prompt)
        self.assertIn('"read_cards"', helpers)
        self.assertIn('"write_cards"', helpers)
        self.assertIn("The host merges disjoint concurrent saves", helpers)
        self.assertIn("all incident endpoints are in scope", helpers)
        self.assertNotIn("and its place in SetLayout", prompt)
        self.assertIn("without SetLayout coordinates", shared["saveNote"])
        self.assertIn("input wire or delete/change", shared["saveNote"])


if __name__ == "__main__":
    unittest.main()
