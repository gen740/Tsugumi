"""Offline grading regressions. Run python3 -m unittest discover -s tests/prompt."""
import json
import unittest

from inspect_fixture import reply
from inspect_probe import summarize


def tool(name="project_inspect", **fields):
    return {"id": "t", "type": "mcp_tool_call", "server": "tsugumi", "tool": name,
            "status": "completed", "arguments": {},
            "result": reply({"method": "tools/call", "params": {"name": name}}), **fields}


def transcript(*items, completed=True):
    events = [{"type": "item.completed", "item": item} for item in items]
    events.append({"type": "item.completed", "item": {"id": "a", "type": "agent_message", "text": "43"}})
    if completed:
        events.append({"type": "turn.completed", "usage": {"input_tokens": 500}})
    return "\n".join(map(json.dumps, events))


class InspectProbeTests(unittest.TestCase):
    def test_strict_snapshot_call(self):
        def call(name, args):
            return reply({"method": "tools/call", "params": {"name": name, "arguments": args}})
        result = call("project_inspect", {})
        self.assertFalse(result["isError"])
        body = json.loads(result["content"][0]["text"])
        self.assertEqual(body["reply"], "ProjectSnapshot")
        definition = body["value"]["definitions"][0]
        self.assertEqual(json.loads(definition["card_json"])["params"][0]["data"], 43)
        self.assertTrue(call("project_inspect", {"id": "initial"})["isError"])
        self.assertTrue(call("project_save", {})["isError"])

    def test_unexpected_actions_fail_even_if_reply_says_43(self):
        for case, calls in [("concept", [tool()]), ("project", []), ("project", [tool("project_save", status="failed")]),
                            ("project", [tool(arguments={"id": "initial"})])]:
            with self.subTest(case=case, calls=calls):
                self.assertEqual(summarize(case, transcript(*calls))["outcome"], "fail")

    def test_redundant_successful_read_is_an_efficiency_cost(self):
        report = summarize("project", transcript(tool(), tool(id="t2")))
        self.assertEqual(report["outcome"], "needs_review")
        self.assertEqual(report["extra_inspections"], 1)

    def test_failed_required_inspection_or_incomplete_turn_is_invalid(self):
        for text in [transcript(tool(status="failed")), transcript(tool(result={"isError": True})),
                     transcript(tool(result={"content": [{"type": "text", "text": "unexpected reply"}]})),
                     transcript(tool(status="in_progress"), completed=False)]:
            report = summarize("project", text)
            self.assertEqual(report["outcome"], "invalid")
            self.assertEqual(len(report["calls"]), 1)

    def test_one_call_per_id_and_manual_semantic_review(self):
        started = {"type": "item.started", "item": tool(status="in_progress")}
        report = summarize("project", json.dumps(started) + "\n" + transcript(tool()))
        self.assertEqual(report["outcome"], "needs_review")
        self.assertEqual(len(report["calls"]), 1)
        self.assertEqual(report["usage"]["input_tokens"], 500)
        self.assertEqual(summarize("concept", transcript())["outcome"], "needs_review")


if __name__ == "__main__":
    unittest.main()
