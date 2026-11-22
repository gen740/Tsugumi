"""Encode independent value fixtures with the public FlatBuffers compiler."""

import json
import re
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


def reference_json(directory, schema, payload, *, root_type, non_finite=False):
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
    text_value = (directory / "value.json").read_text()
    if non_finite:
        # flatc prints nan/inf as unquoted tokens. Leave JSON strings alone.
        text_value = re.sub(r'"(?:[^"\\]|\\.)*"|(?<![A-Za-z])(?:-inf|inf|nan)(?![A-Za-z])',
                            lambda m: {"nan": "NaN", "inf": "Infinity", "-inf": "-Infinity"}.get(m[0], m[0]),
                            text_value)
    return json.loads(text_value, strict=False)


def recorded_input_json(client, directory, run_id, card, *, invocation=1, port="in"):
    """Decode a small recorded input through its public schema and value bytes."""
    result = client.call("RunRecordRequest", runId=run_id, card=path(*card.split("/")),
                         invocation=invocation)
    value = next(value for value in result.record.inputs if text(value.name) == port)
    return reference_json(directory, bytes(value.schemaBfbs), bytes(value.inlineBytes),
                          root_type=text(value.rootType))


def collected_items(client, directory, run_id, card="collect"):
    """Decode Collect envelopes, using the connected source type only for the member payloads."""
    from .observations import read_output

    record = client.call("RunRecordRequest", runId=run_id, card=path(card)).record
    value, = record.outputs
    payload = read_output(client, run_id, card=card)
    items = reference_json(directory, bytes(value.schemaBfbs), payload,
                           root_type=text(value.rootType))["items"]
    if not items:
        return []
    source, = client.call("InputTypesRequest", card=path(card)).inputs
    result = []
    for index, item in enumerate(items):
        if item["kind"] == "Path":
            assert not item.get("bytes")
            result.append(item["path"])
        else:
            assert item["kind"] == "FlatBuffer"
            assert "path" not in item
            result.append(reference_json(directory / str(index), bytes(source.type.schemaBfbs),
                                         bytes(item["bytes"]), root_type=text(source.type.rootType)))
    return result
