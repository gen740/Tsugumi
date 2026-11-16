"""UI retries use the original edit and a new inspection, without host receipts."""
import hashlib
import json
import os
import select
import socket
import struct
import tempfile
import threading
import uuid

import pytest

from harness.host import Host
from harness.remote_worker import exact
from tsugumi_client import change, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.RPC.Message import MessageT
from Tsugumi.RPC.Payload import Payload
from Tsugumi.RPC.ProjectSaveRequest import ProjectSaveRequestT
from Tsugumi.RPC.ReplyPayload import ReplyPayload


def register(host, daemon, endpoint=None):
    host.call("RegisterProjectRequest", projectId=daemon.ready["project_id"],
              endpoint=endpoint or "unix:" + daemon.unix, token=daemon.ready["token"],
              path=str(daemon.project))


def base(client):
    snapshot = client.call("ProjectInspectRequest")
    files = {"tsugumi.json": json.loads(text(snapshot.projectJson)),
             "layout.json": json.loads(text(snapshot.layoutJson))}
    for d in snapshot.definitions:
        for field, name in (("cardJson", "_card.json"), ("metaJson", "_meta.json"), ("linkJson", "_link.json")):
            if getattr(d, field):
                files[f"definitions/{text(d.id)}/{name}"] = json.loads(text(getattr(d, field)))
    sources = [{"path": text(s.path), "hash": text(s.hash)} for s in snapshot.sources]
    # This fixture imports a nested workflow too: capture its document bodies
    # exactly as clients do, while binary copied sources need only their hash.
    for source in sources:
        if source["path"].startswith("externals/") and source["path"].endswith(".json"):
            files[source["path"]] = json.loads(bytes(client.call("ProjectFileReadRequest", path=source["path"], atRev=snapshot.rev).bytes))
    return snapshot, json.dumps({"snapshot": {"rev": str(snapshot.rev), "sources": sources}, "files": files})


def request(snapshot, changes):
    save = ProjectSaveRequestT()
    save.baseRev = snapshot.rev
    save.opId = str(uuid.uuid4())
    save.origin = "ui:retry-test"
    save.changes = changes
    return save


class LoseSaveReply:
    """Forward public core RPC, killing host after SaveResult before forwarding it."""
    def __init__(self, daemon, host):
        self.daemon = daemon
        self.host = host
        self.stopped = threading.Event()
        self.errors = []
        self.directory = tempfile.TemporaryDirectory(prefix="ul-", dir=os.environ.get("TSUGUMI_SOCKET_DIR", "/tmp"))
        self.path = self.directory.name + "/rpc"
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(self.path)
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.done = threading.Event()
        self.workers = []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.done.is_set():
            try:
                client, _ = self.listener.accept()
            except TimeoutError:
                continue
            worker = threading.Thread(target=self.forward, args=(client,), daemon=True)
            self.workers.append(worker)
            worker.start()

    def forward(self, client):
        try:
            with client, socket.socket(socket.AF_UNIX) as core:
                core.connect(self.daemon.unix)
                with client.makefile("rb", buffering=0) as incoming, core.makefile("rb", buffering=0) as outgoing:
                    while not self.done.is_set():
                        ready, _, _ = select.select([client, core], [], [], 0.1)
                        for source in ready:
                            stream = outgoing if source is core else incoming
                            header = exact(stream, 4)
                            size, = struct.unpack("<I", header)
                            body = exact(stream, size)
                            if source is core:
                                message = MessageT.InitFromPackedBuf(body)
                                if message.payloadType == Payload.Reply and message.payload.valueType == ReplyPayload.SaveResult:
                                    self.host.process.kill()
                                    self.host.process.wait(timeout=5)
                                    self.stopped.set()
                                    return
                            (client if source is core else core).sendall(header + body)
        except (EOFError, ConnectionError, OSError):
            pass
        except BaseException as error:
            self.errors.append(error)
            self.stopped.set()

    def close(self):
        self.done.set()
        self.thread.join(timeout=5)
        self.listener.close()
        for worker in self.workers:
            worker.join(timeout=5)
        self.directory.cleanup()
        assert not self.errors


@pytest.mark.parametrize("entry", ["edit", "import", "removal", "source removal", "definition removal", "layout"])
def test_retry_after_core_applied_before_host_reply(core_factory, tmp_path, entry):
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    # Start from an existing definition with a source, ready for removals.
    snap = client.call("ProjectInspectRequest")
    graph = json.loads(text(snap.projectJson))
    graph["canvas"]["cards"] = [{"id": "a"}]
    client.call("ProjectSaveRequest", baseRev=snap.rev, opId=str(uuid.uuid4()), origin="setup", changes=[
        change("SetProject", json=json.dumps(graph)),
        change("SetLayout", json='{"cards":{"a":{"x":0,"y":0}}}'),
        change("PutDefinition", id="a", cardJson='{"template":"draft"}'),
        change("PutSource", path="definitions/a/copied.bin", bytes=b"\x00\xffcopied")])
    snap, captured = base(client)
    if entry == "edit":
        graph["name"] = "mine"
        changes = [change("SetProject", json=json.dumps(graph))]
    elif entry == "import":
        graph["canvas"]["cards"].append({"id": "imported-fixed-id"})
        changes = [change("SetProject", json=json.dumps(graph)),
                   change("PutDefinition", id="imported-fixed-id", cardJson="", linkJson='{"external":"imported"}'),
                   change("PutSource", path="externals/imported/tsugumi.json", bytes=b'{"name":"inner","canvas":{"cards":[{"id":"child"}],"connections":[]}}'),
                   change("PutSource", path="externals/imported/layout.json", bytes=b'{}'),
                   change("PutSource", path="externals/imported/definitions/child/_card.json", bytes=b'{"template":"draft"}'),
                   change("PutSource", path="externals/imported/asset.bin", bytes=b"\x00\xffimported"),
                   change("PutSource", path="definitions/a/copied.bin", baseHash=hashlib.sha256(b"\x00\xffcopied").hexdigest(), bytes=b"replacement")]
    elif entry in ("removal", "definition removal"):
        graph["canvas"]["cards"] = []
        changes = [change("SetProject", json=json.dumps(graph)), change("RemoveDefinition", id="a")]
        if entry == "definition removal":
            # A previously unused definition exercises a removal-only retry.
            client.call("ProjectSaveRequest", baseRev=snap.rev, opId=str(uuid.uuid4()), origin="setup",
                        changes=[change("SetProject", json=json.dumps(graph))])
            snap, captured = base(client)
            changes = [change("RemoveDefinition", id="a")]
    elif entry == "source removal":
        changes = [change("RemoveSource", path="definitions/a/copied.bin", baseHash=hashlib.sha256(b"\x00\xffcopied").hexdigest())]
    else:
        changes = [change("SetLayout", json='{"cards":{"a":{"x":200,"y":100}}}')]
    save = request(snap, changes)
    directory = tmp_path / "host"
    host = Host(directory)
    proxy = LoseSaveReply(daemon, host)
    try:
        register(host, daemon, "unix:" + proxy.path)
        with pytest.raises((EOFError, ConnectionError, OSError)):
            host.call("UISaveRequest", project=daemon.ready["project_id"], baseJson=captured, save=save)
        assert proxy.stopped.wait(5), "SaveResult was not intercepted"
        applied = client.call("ProjectInspectRequest")
        assert applied.rev == snap.rev + 1
    finally:
        host.close()
        proxy.close()
    (directory / "host.ready").unlink(missing_ok=True)
    host = Host(directory)
    try:
        # The saved registry still points at the closed proxy. Replace it before retry.
        register(host, daemon)
        retried = host.call("UISaveRequest", project=daemon.ready["project_id"], baseJson=captured, save=save)
        assert retried.reply.valueType == ReplyPayload.SaveResult
        assert retried.reply.value.rev == applied.rev
        assert retried.reply.value.delta is None
        latest = client.call("ProjectInspectRequest")
        assert latest.rev == applied.rev
        assert text(latest.projectJson) == text(applied.projectJson)
        assert text(latest.layoutJson) == text(applied.layoutJson)
        assert [(text(s.path), text(s.hash)) for s in latest.sources] == [(text(s.path), text(s.hash)) for s in applied.sources]
        assert [text(d.id) for d in latest.definitions] == [text(d.id) for d in applied.definitions]
        if entry == "import":
            assert json.loads(text(latest.layoutJson))["cards"]["imported-fixed-id"]
            assert (daemon.project / "externals/imported/asset.bin").read_bytes() == b"\x00\xffimported"
            assert (daemon.project / "definitions/a/copied.bin").read_bytes() == b"replacement"
    finally:
        host.close()


def test_host_layout_rejection_is_a_typed_ui_failure(core_factory, tmp_path):
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    snap, captured = base(client)
    host = Host(tmp_path / "host")
    try:
        register(host, daemon)
        save = request(snap, [change("SetLayout", json='{"cards":{"a":{"x":0}}}')])
        result = host.call("UISaveRequest", project=daemon.ready["project_id"], baseJson=captured, save=save)
        assert result.reply.valueType == ReplyPayload.Failure
        assert result.reply.value.kind == FailureKind.invalid_document
        assert text(result.reply.value.diagnostic)
        assert client.call("ProjectInspectRequest").rev == snap.rev
        # The same shared service remains usable after the rejection.
        host.call("LayoutValidateRequest", json="{}")
    finally:
        host.close()
