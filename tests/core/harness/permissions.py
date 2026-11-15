"""Public permission requests shared by document and Nix source scenarios."""

from tsugumi_client import RpcFailure, new_id, text


def permissions(e: RpcFailure) -> list[str]:
    return [text(kv.value) for kv in e.failure.args if text(kv.key) == "permission"]


def save(c, changes, origin="agent", approved=None, check=None):
    """Saves as the host service does for the Agent (the check on) or as a person's
    client does (off); `origin` is only the label of who edited."""
    if check is None:
        check = origin == "agent"
    rev = c.call("ProjectInspectRequest").rev
    extra = {"approvedPermissions": approved} if approved is not None else {}
    return c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin=origin, changes=changes, checkPermissions=check, **extra)
