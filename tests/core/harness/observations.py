"""Read public RPC values and verify their paged transport."""

import hashlib
import struct

import flatbuffers.number_types
from flatbuffers.table import Table

from tsugumi_client import path, text
from Tsugumi.Data.ValueSide import ValueSide


def read_pages(client, request, *, page_bytes=31, **fields):
    """Read all pages, checking progress, stable identity, and the final digest."""
    content = bytearray()
    offset = 0
    digest = None
    while True:
        reply = client.call(request, offset=offset, limitBytes=page_bytes, **fields)
        chunk = bytes(reply.bytes)
        assert reply.offset == offset
        assert reply.nextOffset == offset + len(chunk)
        assert reply.eof or chunk, "a non-final public page must advance"
        digest = text(reply.hash) if digest is None else digest
        assert text(reply.hash) == digest
        content.extend(chunk)
        offset = reply.nextOffset
        if reply.eof:
            assert reply.sizeBytes == len(content)
            assert hashlib.sha256(content).hexdigest() == digest
            return bytes(content)


def read_output(client, run_id, *, card="work", port="out"):
    """Read one recorded output through RunValueRead."""
    return read_pages(client, "RunValueReadRequest", runId=run_id, card=path(card),
                      invocation=1, side=ValueSide.output, port=port)


def decode_scalar(payload, kind="string", *, field=0):
    """Decode a scalar field of a fixture, independently of its encoder."""
    table = Table(payload, struct.unpack_from("<I", payload)[0])
    offset = table.Offset(4 + 2 * field)
    if kind == "string":
        return table.String(table.Pos + offset).decode()
    flags = {
        "int64": flatbuffers.number_types.Int64Flags,
        "uint64": flatbuffers.number_types.Uint64Flags,
        "double": flatbuffers.number_types.Float64Flags,
        "bool": flatbuffers.number_types.BoolFlags,
    }
    return table.Get(flags[kind], table.Pos + offset) if offset else 0
