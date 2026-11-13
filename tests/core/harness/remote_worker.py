"""Generated remote-worker objects and bounded stream helpers for test peers."""
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.environ.get("TSUGUMI_PY_BINDINGS", str(Path(__file__).resolve().parents[3] / "build/generated/python")))

import flatbuffers
from Tsugumi.RemoteWorker.Ack import AckT
from Tsugumi.RemoteWorker.BrokerRequest import BrokerRequestT
from Tsugumi.RemoteWorker.BlobChunk import BlobChunkT
from Tsugumi.RemoteWorker.CancelRequest import CancelRequestT
from Tsugumi.RemoteWorker.Capabilities import CapabilitiesT
from Tsugumi.RemoteWorker.Catalog import CatalogT
from Tsugumi.RemoteWorker.CatalogRequest import CatalogRequestT
from Tsugumi.RemoteWorker.ErrorCode import ErrorCode
from Tsugumi.RemoteWorker.Failure import FailureT
from Tsugumi.RemoteWorker.GetBlobRequest import GetBlobRequestT
from Tsugumi.RemoteWorker.JobSpec import JobSpecT
from Tsugumi.RemoteWorker.JobState import JobState
from Tsugumi.RemoteWorker.JobStatus import JobStatusT
from Tsugumi.RemoteWorker.Message import MessageT
from Tsugumi.RemoteWorker.Payload import Payload
from Tsugumi.RemoteWorker.ProbeRequest import ProbeRequestT
from Tsugumi.RemoteWorker.PutBlobRequest import PutBlobRequestT
from Tsugumi.RemoteWorker.ReleaseRequest import ReleaseRequestT
from Tsugumi.RemoteWorker.Reply import ReplyT
from Tsugumi.RemoteWorker.ReplyBody import ReplyBody
from Tsugumi.RemoteWorker.Request import RequestT
from Tsugumi.RemoteWorker.RequestBody import RequestBody
from Tsugumi.RemoteWorker.ResultFile import ResultFileT
from Tsugumi.RemoteWorker.ResultManifest import ResultManifestT
from Tsugumi.RemoteWorker.StatusRequest import StatusRequestT
from Tsugumi.RemoteWorker.SubmitRequest import SubmitRequestT
from Tsugumi.RemoteWorker.Target import TargetT

__all__ = ['BrokerRequestT', 'AckT', 'BlobChunkT', 'CancelRequestT', 'CapabilitiesT', 'CatalogRequestT', 'CatalogT', 'ErrorCode', 'FailureT', 'GetBlobRequestT', 'JobSpecT', 'JobState', 'JobStatusT', 'MessageT', 'Payload', 'ProbeRequestT', 'PutBlobRequestT', 'ReleaseRequestT', 'ReplyBody', 'ReplyT', 'RequestBody', 'RequestT', 'ResultFileT', 'ResultManifestT', 'StatusRequestT', 'SubmitRequestT', 'TargetT', 'exact', 'failure', 'frame', 'pack', 'read', 'reply', 'request', 'string']

MAX_FRAME = 2 << 20


def pack(obj, identifier=None):
    b = flatbuffers.Builder(128)
    b.Finish(obj.Pack(b), file_identifier=identifier)
    return bytes(b.Output())


def frame(message):
    data = pack(message, b"TSR1")
    assert len(data) <= MAX_FRAME
    return struct.pack("<I", len(data)) + data


def exact(stream, size):
    result = bytearray()
    while len(result) < size:
        part = stream.read(size - len(result))
        if not part:
            raise EOFError("truncated remote worker stream")
        result.extend(part)
    return bytes(result)


def read(stream):
    size, = struct.unpack("<I", exact(stream, 4))
    assert 12 <= size <= MAX_FRAME
    return MessageT.InitFromPackedBuf(exact(stream, size))


def request(body_type, body, **routing):
    return MessageT(payloadType=Payload.Request, payload=RequestT(bodyType=body_type, body=body, **routing))


def reply(body_type, body):
    return MessageT(payloadType=Payload.Reply, payload=ReplyT(bodyType=body_type, body=body))


def failure(code, message):
    return reply(ReplyBody.Failure, FailureT(code=code, message=message))


def string(value):
    return value.decode() if isinstance(value, bytes) else value
