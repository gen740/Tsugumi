"""Offline grading regressions. Run python3 -m unittest discover -s tests/prompt."""
import json
import unittest

from inspect_fixture import PARAM_BYTES, PARAM_PATH, reply
from inspect_probe import summarize


def tool(name="project_file_read", **fields):
    arguments = fields.pop("arguments", {"path": PARAM_PATH} if name == "project_file_read" else {})
    result = reply({"method": "tools/call", "params": {"name": name, "arguments": arguments}})
    return {"id": fields.pop("id", "t"), "type": "mcp_tool_call", "server": "tsugumi", "tool": name,
            "status": fields.pop("status", "completed"), "arguments": arguments, "result": fields.pop("result", result), **fields}


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
        self.assertEqual(json.loads(definition["card_json"]), {"type": "code"})
        self.assertIn("definitions/initial/params.nix",
                      [source["path"] for source in body["value"]["sources"]])
        self.assertTrue(call("project_inspect", {"id": "initial"})["isError"])
        self.assertTrue(call("project_save", {})["isError"])
        file = call("project_file_read", {"path": PARAM_PATH})
        self.assertEqual(bytes(json.loads(file["content"][0]["text"])["value"]["bytes"]), PARAM_BYTES)

    def test_unexpected_actions_fail_even_if_reply_says_43(self):
        for case, calls in [("concept", [tool()]), ("project", []), ("project", [tool("project_save", status="failed")]),
                            ("project", [tool(arguments={"id": "initial"})])]:
            with self.subTest(case=case, calls=calls):
                self.assertEqual(summarize(case, transcript(*calls))["outcome"], "fail")

    def test_redundant_successful_read_is_an_efficiency_cost(self):
        report = summarize("project", transcript(tool(), tool(id="t2")))
        self.assertEqual(report["outcome"], "needs_review")
        self.assertEqual(report["extra_reads"], 1)

    def test_failed_retry_does_not_erase_observed_evidence(self):
        report = summarize("project", transcript(tool(status="failed"), tool(id="t2")))
        self.assertEqual(report["outcome"], "needs_review")
        self.assertEqual(report["extra_reads"], 1)
        self.assertEqual(report["failed_reads"], 1)

    def test_evidence_allows_extra_metadata_and_rejects_a_wrong_parameter(self):
        result = tool()
        block = result["result"]["content"][0]
        body = json.loads(block["text"])
        body["optional_metadata"] = {"anything": True}
        param = body["value"]
        param["optional_metadata"] = True
        block["text"] = json.dumps(body)
        self.assertEqual(summarize("project", transcript(result))["outcome"], "needs_review")
        param["bytes"] = list(b"{ value = 27; }\n")
        block["text"] = json.dumps(body)
        self.assertEqual(summarize("project", transcript(result))["outcome"], "invalid")

    def test_snapshot_metadata_alone_cannot_answer_the_question(self):
        self.assertEqual(summarize("project", transcript(tool("project_inspect")))["outcome"], "invalid")

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
