from datetime import datetime, timezone
from shutil import copytree

from card import Artifact, Input, Output


def run(input: Input) -> Output:
    source = input.require("report").open()
    artifact, directory = Artifact.dir()
    copytree(source, directory, dirs_exist_ok=True)
    (directory / "ARCHIVED_AT").write_text(
        datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ\n"),
        encoding="utf-8",
    )
    return Output(archive=artifact)
