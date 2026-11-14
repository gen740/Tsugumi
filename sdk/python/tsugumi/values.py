"""Ordinary returned values to FlatBuffers, using the Card's generated schema.

Builders belong to this module. Authors return port mappings containing dicts,
scalars, sequences, NumPy arrays or data frames, including nested messages.
"""
from __future__ import annotations

from collections.abc import Mapping
from functools import cache
import importlib
import json
from pathlib import Path

from tsugumi import Failure


@cache
def _schema():
    return json.loads((Path(__file__).resolve().parent.parent / "_outputs.json").read_text())


def _camel(name):
    first, *rest = name.split("_")
    return first + "".join(part[:1].upper() + part[1:] for part in rest)


def _enum(schema, index, value):
    if isinstance(value, str):
        enum = schema["enums"][index]
        if " " in value:
            # FlatBuffers bit flags use a space-separated set of names.
            result = 0
            for name in value.split():
                result |= _enum(schema, index, name)
            return result
        for item in enum["values"]:
            if item["name"] == value:
                return item["value"]
        raise ValueError(f"unknown {enum['name']} member {value!r}")
    return int(value)


def _value(schema, type_, value, discriminator=None):
    base = type_["base_type"]
    if value is None:
        return None
    if base == "Obj":
        return _object(schema, type_["index"], value)
    if base == "Union":
        tag = _enum(schema, type_["index"], discriminator)
        member = next((x for x in schema["enums"][type_["index"]]["values"] if x["value"] == tag), None)
        if member is None or tag == 0:
            raise ValueError("a union value needs its non-NONE discriminator")
        return _value(schema, member["union_type"], value)
    if base in ("Vector", "Array"):
        element = {**type_, "base_type": type_["element"]}
        if hasattr(value, "to_dict") and hasattr(value, "columns"):
            value = value.to_dict(orient="records")
        if base == "Array" and len(value) != type_["fixed_length"]:
            raise ValueError("a fixed array has the wrong length")
        if element["base_type"] == "UByte" and isinstance(value, (bytes, bytearray, memoryview)):
            return bytes(value)
        if hasattr(value, "dtype") and element["base_type"] not in ("Obj", "String", "Union"):
            # Match the contract, not the caller's platform-dependent array dtype.
            import numpy as np
            dtype = {"Bool": "?", "Byte": "i1", "UByte": "u1", "Short": "i2", "UShort": "u2",
                     "Int": "i4", "UInt": "u4", "Long": "i8", "ULong": "u8", "Float": "f4", "Double": "f8"}[element["base_type"]]
            array = np.asarray(value)
            if array.ndim != 1:
                raise ValueError("a vector must have one dimension")
            # Integer conversion must not silently wrap or truncate a value.
            if element["base_type"] not in ("Float", "Double", "Bool"):
                bounds = np.iinfo(dtype)
                if array.size and (np.any(array < bounds.min) or np.any(array > bounds.max) or np.any(array != np.floor(array))):
                    raise ValueError("integer array values are outside the contract's range")
            return np.asarray(array, dtype=dtype)
        return [_value(schema, element, item) for item in value]
    if base == "String":
        if isinstance(value, bytes):
            return value.decode("utf-8")
        if not isinstance(value, str):
            raise ValueError("a string field needs text")
        return value
    if type_["index"] >= 0:
        return _enum(schema, type_["index"], value)
    if base == "Bool":
        if not isinstance(value, bool) and type(value).__name__ != "bool":
            raise ValueError("a bool field needs a boolean")
        return bool(value)
    if base in ("Float", "Double"):
        return float(value)
    integer = int(value)
    if integer != value:
        raise ValueError("an integer field needs an exact integer")
    return integer


def _object(schema, index, value):
    object_ = schema["objects"][index]
    fields = object_["fields"]
    if hasattr(value, "to_dict") and hasattr(value, "columns"):
        value = value.to_dict(orient="records" if len(fields) == 1 and fields[0]["type"]["element"] == "Obj" else "list")
    generated = not isinstance(value, Mapping) and hasattr(value, "Pack")
    if generated:
        value = {f["name"]: getattr(value, _camel(f["name"]), None) for f in fields}
    elif not isinstance(value, Mapping):
        if len(fields) != 1:
            raise ValueError(f"{object_['name']} needs a mapping by field")
        value = {fields[0]["name"]: value}
    unknown = value.keys() - {f["name"] for f in fields}
    if unknown:
        raise ValueError(f"unknown fields of {object_['name']}: {sorted(unknown)}")
    module = importlib.import_module(object_["name"])
    result = getattr(module, object_["name"].rsplit(".", 1)[1] + "T")()
    for field in fields:
        name = field["name"]
        if name not in value or value[name] is None:
            if field["required"]:
                raise ValueError(f"required field {name} is missing")
            continue
        discriminator = value.get(name + "_type") if field["type"]["base_type"] == "Union" else None
        setattr(result, _camel(name), _value(schema, field["type"], value[name], discriminator))
    return result


def encode(port, value):
    from tsugumi import contract
    import flatbuffers
    found = contract.port(port, "outputs")
    if found is None:
        raise Failure("output_invalid", f"unknown output port {port}")
    if found["kind"] in ("file", "directory") and isinstance(value, (str, Path)):
        value = {"value": str(value)}
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value)
    if hasattr(value, "Pack") or hasattr(value, "Output"):
        from tsugumi import message
        return message(value)
    schema = _schema()
    try:
        index = next(i for i, item in enumerate(schema["objects"]) if item["name"] == found["type"])
        result = _object(schema, index, value)
        builder = flatbuffers.Builder(1024)
        builder.Finish(result.Pack(builder))
        return bytes(builder.Output())
    except Exception as error:
        raise Failure("output_invalid", f"output {port}: {error}") from error
