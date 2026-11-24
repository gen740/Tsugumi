"""Live view helpers shared by the public Core fixtures."""

import uuid

from Tsugumi.RPC.NotificationPayload import NotificationPayload
from tsugumi_client import text

EMPTY_OUTPUTS = "table Outputs {} root_type Outputs;"
ZERO_ROUTE = str(uuid.UUID(int=0))
MESSAGE_MAX = 1 << 24
# These C fixtures exercise the relay across processes. They make no network
# calls and run without host-specific sandbox tools.
NATIVE_VIEW_SETTINGS = {
    "type": "code",
    "sandbox": dict(filesystem=False, pid=False, ipc=False, identity=False),
    "grants": dict(network=True),
}


def open_view(client, *, timeout=15):
    opened = client.wait_notification(
        lambda n: n.valueType == NotificationPayload.ViewOpened, timeout=timeout
    ).value
    assert text(opened.session) == str(uuid.UUID(text(opened.session)))
    return opened


def display(client, session):
    return client.wait_notification(
        lambda n: n.valueType == NotificationPayload.DisplayUpdated
        and text(n.value.display.session) == session, timeout=15
    ).value.display


def return_credit(client, subscription, update):
    client.call("ViewCreditRequest", subscriptionId=subscription,
                session=text(update.session), bytes=len(update.bytes), creditId=update.creditId)


def declaration(blob, name="page", kind="html"):
    return {"blob": str(blob), "name": name, "type": kind}
