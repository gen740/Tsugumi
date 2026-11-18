"""Delegate schemas and executable example arguments; no model or socket is needed."""
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class DelegateContractTests(unittest.TestCase):
    def check_value(self, value, schema):
        kinds = {"object": dict, "array": list, "string": str, "integer": int}
        kind = schema["type"]
        self.assertIs(type(value), kinds[kind])
        if kind == "object":
            properties = schema["properties"]
            self.assertLessEqual(set(schema.get("required", ())), set(value))
            if schema.get("additionalProperties") is False:
                self.assertLessEqual(set(value), set(properties))
            for name in value.keys() & properties.keys():
                self.check_value(value[name], properties[name])
        elif kind == "array":
            self.assertGreaterEqual(len(value), schema.get("minItems", 0))
            for item in value:
                self.check_value(item, schema["items"])
        elif kind == "string":
            self.assertGreaterEqual(len(value), schema.get("minLength", 0))
            if "maxLength" in schema:
                self.assertLessEqual(len(value), schema["maxLength"])
        elif "minimum" in schema:
            self.assertGreaterEqual(value, schema["minimum"])

    def test_helpers_skill_example_uses_only_supported_task_arguments(self):
        prompt = (ROOT / "hostd/agent/Shared/helpers-skill.md").read_text()
        schema = json.loads((ROOT / "tests/fixtures/agent-contract.json").read_text())["delegateSchema"]
        examples = list(re.finditer(r"^[ \t]*delegate\b\s*(?=\{)", prompt, re.MULTILINE))
        self.assertTrue(examples, "helpers skill must include an executable delegate example")
        for example in examples:
            arguments, _ = json.JSONDecoder().raw_decode(prompt, example.end())
            self.check_value(arguments, schema)
