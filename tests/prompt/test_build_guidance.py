"""Offline: the Agent's instructions state what building a project otherwise teaches by failing.
Run python3 -m unittest discover -s tests/prompt."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROMPT = (ROOT / "hostd/agent/Shared/system-prompt.md").read_text()
REVIEW = (ROOT / "hostd/agent/Shared/review-skill.md").read_text()
FBS = (ROOT / "protocol/tsugumi.fbs").read_text()
ENUMS = (ROOT / "protocol/tsugumi_enums.fbs").read_text()


def flat(text):
    return re.sub(r"\s+", " ", text)


class BuildGuidance(unittest.TestCase):
    def test_a_new_card_lists_its_files(self):
        text = flat(PROMPT)
        for name in ("_card.json", "README.md", "default.nix", "_outputs.fbs", "params.nix", "_inputs.fbs"):
            self.assertIn(name, text)
        self.assertIn("Every Card's folder has _card.json and README.md", text)

    def test_sandbox_tools_come_from_the_sdk_builder(self):
        text = flat(PROMPT)
        self.assertIn("tools = [ pkgs.bash pkgs.coreutils ]", text)
        self.assertIn("there is no /bin/sh", text)
        self.assertIn("Work done in the Card's Nix build is part of the run too", text)

    def test_cards_keep_command_output_and_report_progress(self):
        text = flat(PROMPT)
        self.assertIn("passes the command's stdout and stderr through", text)
        self.assertIn("prints its progress", text)
        self.assertIn("stdout and stderr do not reach the Card's log", flat(REVIEW))

    def test_runs_in_progress_and_long_runs(self):
        text = flat(PROMPT)
        self.assertIn("edits saved while it runs reach the next run", text)
        self.assertIn("a Card's files the run still evaluates are refused (busy) until it ends", text)
        self.assertIn("ends after your reply brings you back with a note of its outcome", text)
        self.assertIn("a long run is followed by waiting again", text)

    def test_log_streams_named_in_the_prompt_are_the_schema_values(self):
        values = re.search(r"enum LogStream : ubyte \{([^}]*)\}", ENUMS).group(1)
        names = [v.split("=")[0].strip() for v in values.split(",")]
        streams = [n for n in names if n != "unspecified"]
        self.assertIn("takes stream " + ", ".join(streams[:-1]) + " or " + streams[-1], flat(PROMPT))
        log_range = flat(FBS[FBS.index("table LogRangeRequest"):][:1500])
        for stream in streams:
            self.assertIn(stream, log_range)
        value_read = flat(FBS[FBS.index("table RunValueReadRequest"):][:600])
        self.assertIn("Required: the invocation number", value_read)


if __name__ == "__main__":
    unittest.main()
