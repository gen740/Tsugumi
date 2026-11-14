"""Encode independent value fixtures with the public FlatBuffers compiler."""

import json
import shutil
import subprocess

from tsugumi_client import path, text


def encode_message(directory, schema, value, *, root_type):
    """Return a message buffer without importing any core implementation."""
    directory.mkdir(parents=True, exist_ok=True)
    schema_file = directory / "message.fbs"
    value_file = directory / "value.json"
    schema_file.write_text(schema)
    value_file.write_text(value if isinstance(value, str) else json.dumps(value, ensure_ascii=False))
    compiler = shutil.which("flatc")
    if compiler is None:
        raise RuntimeError("flatc is required to encode RPC value fixtures")
    subprocess.run([compiler, "--binary", "--strict-json", "--root-type", root_type,
                    "-o", str(directory), str(schema_file), str(value_file)],
                   check=True, capture_output=True, text=True, timeout=10)
    return (directory / "value.bin").read_bytes()


def reference_json(directory, schema, payload, *, root_type):
    """Decode a fixture with flatc as an independent oracle for structured values."""
    directory.mkdir(parents=True, exist_ok=True)
    schema_file = directory / ("message.bfbs" if isinstance(schema, bytes) else "message.fbs")
    value_file = directory / "value.bin"
    if isinstance(schema, bytes):
        schema_file.write_bytes(schema)
    else:
        schema_file.write_text(schema)
    value_file.write_bytes(payload)
    compiler = shutil.which("flatc")
    if compiler is None:
        raise RuntimeError("flatc is required to decode RPC value fixtures")
    subprocess.run([compiler, "--json", "--strict-json", "--natural-utf8", "--defaults-json",
                    "--raw-binary", "--root-type", root_type, "-o", str(directory),
                    str(schema_file), "--", str(value_file)],
                   check=True, capture_output=True, text=True, timeout=10)
    # Natural-UTF8 output may retain literal control characters in strings.
    return json.loads((directory / "value.json").read_text(), strict=False)


def recorded_input_json(client, directory, run_id, card, *, invocation=1, port="in"):
    """Decode a small recorded input through its public schema and value bytes."""
    result = client.call("RunRecordRequest", runId=run_id, card=path(*card.split("/")),
                         invocation=invocation)
    value = next(value for value in result.record.inputs if text(value.name) == port)
    return reference_json(directory, bytes(value.schemaBfbs), bytes(value.inlineBytes),
                          root_type=text(value.rootType))
