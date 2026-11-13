"""Private FlatBuffers VM adapter double; its child is a real native tsugumid.

No VM is booted. It exercises agentd's adapter protocol, readiness and broker
wiring rather than duplicating environment decisions in the Web host.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.environ["TSUGUMI_PY_BINDINGS"])
import flatbuffers
from Tsugumi.VMHost.Message import Message, MessageT
from Tsugumi.VMHost.Payload import Payload
from Tsugumi.VMHost.Reply import ReplyT
from Tsugumi.VMHost.ReplyPayload import ReplyPayload
from Tsugumi.VMHost.RequestPayload import RequestPayload
from Tsugumi.VMHost.Process import ProcessT
from Tsugumi.VMHost.Failure import FailureT
from Tsugumi.Service.Acknowledgement import AcknowledgementT
from Tsugumi.Host.HostRuntime import HostRuntimeT

support = Path(sys.argv[sys.argv.index("--support") + 1]).resolve()
lock = open(support / "vmhost.lock", "a")
try:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.exit(0)
endpoint = Path("/tmp") / ("tsugumi-vmhost-%d-%s.sock" % (os.getuid(), hashlib.sha256(str(support).encode()).hexdigest()[:24]))
endpoint.unlink(missing_ok=True)
listener = socket.socket(socket.AF_UNIX)
listener.bind(str(endpoint))
endpoint.chmod(0o600)
listener.listen()
Path(os.environ["HOST_PID"]).write_text(str(os.getpid()))
broker_endpoint = ""
processes = {}
controls = {}
mutex = threading.Lock()
sockets = Path(tempfile.mkdtemp(prefix="tsugumi-helper-double-"))

def text(value):
    return value.decode() if isinstance(value, bytes) else value

def receive(peer, length):
    data = b""
    while len(data) < length:
        part = peer.recv(length - len(data))
        if not part:
            return None
        data += part
    return data

def send(peer, id, value, kind):
    reply = ReplyT()
    reply.valueType, reply.value = kind, value
    message = MessageT()
    message.requestId, message.payloadType, message.payload = id, Payload.Reply, reply
    builder = flatbuffers.Builder(1024)
    builder.Finish(message.Pack(builder), file_identifier=b"TSV1")
    data = bytes(builder.Output())
    peer.sendall(struct.pack("<I", len(data)) + data)

def status(key):
    result = ProcessT()
    child = processes.get(key)
    result.running = child is not None and child.poll() is None
    result.pid = child.pid if result.running else 0
    if result.running:
        try:
            result.endpoint = json.loads((controls[key] / "server.ready").read_text())["endpoint"]
        except FileNotFoundError:
            pass
    return result

def serve(peer):
    global broker_endpoint
    with peer:
        while prefix := receive(peer, 4):
            length, = struct.unpack("<I", prefix)
            assert 0 < length <= 16 << 20
            data = receive(peer, length)
            assert Message.MessageBufferHasIdentifier(data, 0)
            message = MessageT.InitFromObj(Message.GetRootAs(data))
            request = message.payload.value
            kind = message.payload.valueType
            with mutex:
                value, tag = AcknowledgementT(), ReplyPayload.Acknowledgement
                if kind == RequestPayload.Run:
                    key = text(request.key)
                    with open(os.environ["HOST_LOG"], "a") as log:
                        log.write(json.dumps({"op": "run", "origins": [text(v) for v in request.origins or []], "archive": request.archiveIncompatibleHistory}) + "\n")
                    if Path(os.environ["HOST_BUSY"]).exists():
                        value, tag = FailureT(), ReplyPayload.Failure
                        value.message, value.recovery = "fixture VM failed", "Retry the virtual machine"
                    else:
                        mounts = {text(m.name): Path(text(m.path)) for m in request.mounts or []}
                        controls[key] = mounts["control"]
                        # Match vmd: native clients (including agentd's readiness
                        # check) have no Origin; browsers still need an allowed one.
                        arguments = [os.environ["TSUGUMID"], "--project", str(mounts["project"]), "--control-dir", str(mounts["control"]),
                                     "--unix", str(sockets / key), "--listen", "127.0.0.1:0", "--allow-missing-origin", "--worker", os.environ["TSUGUMI_WORKER"], "--sdk-dir", os.environ["TSUGUMI_SDK_DIR"]]
                        for origin in request.origins or []:
                            arguments.extend(["--allow-origin", text(origin)])
                        if request.archiveIncompatibleHistory:
                            arguments.append("--archive-incompatible-history")
                        environment = dict(os.environ, TSUGUMI_EXECUTION_BROKER=broker_endpoint, TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE=str(mounts["control"] / "execution-broker-capability"))
                        processes[key] = subprocess.Popen(arguments, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                        value, tag = status(key), ReplyPayload.Process
                elif kind == RequestPayload.Boot:
                    broker_endpoint = text(request.brokerEndpoint)
                elif kind == RequestPayload.ProcessRequest:
                    key = text(request.key)
                    if request.action == 1 and status(key).running:
                        processes[key].terminate()
                        processes[key].wait(25)
                    value, tag = status(key), ReplyPayload.Process
                elif kind == RequestPayload.HostRuntimeRequest:
                    value, tag = HostRuntimeT(), ReplyPayload.HostRuntime
                    value.vmState, value.virtualizationSupported, value.imageAvailable = "running", True, True
                send(peer, message.requestId, value, tag)

def finish(_sig, _frame):
    for child in processes.values():
        if child.poll() is None:
            child.terminate()
    endpoint.unlink(missing_ok=True)
    sys.exit(0)

signal.signal(signal.SIGTERM, finish)
while True:
    peer, _ = listener.accept()
    threading.Thread(target=serve, args=(peer,), daemon=True).start()
