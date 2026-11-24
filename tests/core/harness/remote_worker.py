"""Generated remote-worker objects and bounded stream helpers for test peers."""
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.environ.get("TSUGUMI_PY_BINDINGS", str(Path(__file__).resolve().parents[3] / "build/generated/python")))

import flatbuffers
from Tsugumi.RemoteWorker import BlobChunk
from Tsugumi.RemoteWorker import PutBlobRequest
from Tsugumi.RemoteWorker.Ack import AckT
from Tsugumi.RemoteWorker.BrokerRequest import BrokerRequestT
from Tsugumi.RemoteWorker.BlobChunk import BlobChunkT
from Tsugumi.RemoteWorker.CancelRequest import CancelRequestT
from Tsugumi.RemoteWorker.CardSecret import CardSecretT
from Tsugumi.RemoteWorker.CardSecrets import CardSecretsT
from Tsugumi.RemoteWorker.Capabilities import CapabilitiesT
from Tsugumi.RemoteWorker.Catalog import CatalogT
from Tsugumi.RemoteWorker.CatalogRequest import CatalogRequestT
from Tsugumi.RemoteWorker.ErrorCode import ErrorCode
from Tsugumi.RemoteWorker.Failure import FailureT
from Tsugumi.RemoteWorker.GetBlobRequest import GetBlobRequestT
from Tsugumi.RemoteWorker.JobSecretsRequest import JobSecretsRequestT
from Tsugumi.RemoteWorker.JobSpec import JobSpecT
from Tsugumi.RemoteWorker.JobState import JobState
from Tsugumi.RemoteWorker.JobStatus import JobStatusT
from Tsugumi.RemoteWorker.Invocation import InvocationT
from Tsugumi.RemoteWorker.InvocationFile import InvocationFileT
from Tsugumi.RemoteWorker.JournalState import JournalStateT
from Tsugumi.RemoteWorker.LogEntry import LogEntryT
from Tsugumi.RemoteWorker.Message import Message, MessageT
from Tsugumi.RemoteWorker.Payload import Payload
from Tsugumi.RemoteWorker.ProbeRequest import ProbeRequestT
from Tsugumi.RemoteWorker.PutBlobRequest import PutBlobRequestT
from Tsugumi.RemoteWorker.ReleaseRequest import ReleaseRequestT
from Tsugumi.RemoteWorker.Reply import ReplyT
from Tsugumi.RemoteWorker.ReplyBody import ReplyBody
from Tsugumi.RemoteWorker.Request import RequestT
from Tsugumi.RemoteWorker.RequestBody import RequestBody
from Tsugumi.RemoteWorker.ResultFile import ResultFileT
from Tsugumi.RemoteWorker.ResultEnvironment import ResultEnvironmentT
from Tsugumi.RemoteWorker.ResultManifest import ResultManifestT
from Tsugumi.RemoteWorker.StatusRequest import StatusRequestT
from Tsugumi.RemoteWorker.SubmitRequest import SubmitRequestT
from Tsugumi.RemoteWorker.Target import TargetT

WORKER_CONTRACT = 18

__all__ = ['BrokerRequestT', 'AckT', 'BlobChunkT', 'CancelRequestT', 'CapabilitiesT', 'CardSecretT', 'CardSecretsT', 'CatalogRequestT', 'CatalogT', 'ErrorCode', 'FailureT', 'GetBlobRequestT', 'JobSecretsRequestT', 'JobSpecT', 'JobState', 'JobStatusT', 'MessageT', 'Payload', 'ProbeRequestT', 'PutBlobRequestT', 'ReleaseRequestT', 'ReplyBody', 'ReplyT', 'RequestBody', 'RequestT', 'ResultFileT', 'ResultManifestT', 'StatusRequestT', 'SubmitRequestT', 'TargetT', 'broker_request', 'exact', 'failure', 'frame', 'pack', 'read', 'read_broker_request', 'reply', 'request', 'string']


class _ByteBlobChunk(BlobChunkT):
    def Pack(self, builder):
        # The generated object API prepends each byte separately without NumPy.
        digest = builder.CreateString(self.digest)
        contents = builder.CreateByteVector(self.bytes)
        BlobChunk.Start(builder)
        BlobChunk.AddDigest(builder, digest)
        BlobChunk.AddTotal(builder, self.total)
        BlobChunk.AddOffset(builder, self.offset)
        BlobChunk.AddBytes(builder, contents)
        return BlobChunk.End(builder)


def blob_chunk(digest, total, offset, contents):
    return reply(ReplyBody.BlobChunk, _ByteBlobChunk(
        digest=digest, total=total, offset=offset, bytes=contents,
    ))


class _BytePutBlobRequest(PutBlobRequestT):
    def Pack(self, builder):
        # Archive chunks are bytes already. Pack the whole vector in one go.
        digest = builder.CreateString(self.digest)
        contents = builder.CreateByteVector(self.bytes)
        PutBlobRequest.Start(builder)
        PutBlobRequest.AddDigest(builder, digest)
        PutBlobRequest.AddTotal(builder, self.total)
        PutBlobRequest.AddOffset(builder, self.offset)
        PutBlobRequest.AddBytes(builder, contents)
        return PutBlobRequest.End(builder)


def put_blob_chunk(digest, total, offset, contents):
    return _BytePutBlobRequest(digest=digest, total=total, offset=offset, bytes=contents)


def pack(obj, identifier=None):
    b = flatbuffers.Builder(128)
    b.Finish(obj.Pack(b), file_identifier=identifier)
    return bytes(b.Output())


def log_entries(*entries):
    frames = []
    for stream, message in entries:
        b = flatbuffers.Builder(128)
        b.FinishSizePrefixed(LogEntryT(stream=stream, text=message.encode()).Pack(b))
        frames.append(bytes(b.Output()))
    return b"".join(frames)


def read_logs(buffer):
    offset = 0
    while offset < len(buffer):
        size, = struct.unpack_from("<I", buffer, offset)
        entry = LogEntryT.InitFromPackedBuf(buffer, offset + 4)
        yield entry.stream, bytes(entry.text).decode()
        offset += 4 + size


def frame(message):
    data = pack(message, b"TSR1")
    return struct.pack("<I", len(data)) + data


def exact(stream, size):
    result = bytearray()
    while len(result) < size:
        part = stream.read(min(size - len(result), 65536))
        if not part:
            raise EOFError("truncated remote worker stream")
        result.extend(part)
    return bytes(result)


def read_message(stream):
    size, = struct.unpack("<I", exact(stream, 4))
    return Message.GetRootAs(exact(stream, size), 0)


def read(stream):
    return MessageT.InitFromObj(read_message(stream))


def request(body_type, body, **routing):
    return MessageT(payloadType=Payload.Request, payload=RequestT(bodyType=body_type, body=body, **routing))


def broker_request(capability, body_type, body, **routing):
    """The local core-to-hostd envelope; worker requests carry no capability."""
    return MessageT(payloadType=Payload.BrokerRequest, payload=BrokerRequestT(
        capability=capability, request=RequestT(bodyType=body_type, body=body, **routing)))


def read_broker_request(stream):
    """Test doubles sit at hostd's private endpoint, after project authorization."""
    return read(stream).payload.request


def reply(body_type, body):
    return MessageT(payloadType=Payload.Reply, payload=ReplyT(bodyType=body_type, body=body))


def failure(code, message):
    return reply(ReplyBody.Failure, FailureT(code=code, message=message))


def string(value):
    return value.decode() if isinstance(value, bytes) else value
