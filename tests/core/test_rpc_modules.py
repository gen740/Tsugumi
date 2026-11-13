"""Module retrieval is an immutable, bounded RPC read, never a UI store read."""
import pytest
from tsugumi_client import RpcFailure


@pytest.mark.parametrize("identity", ["/etc", "/nix/store", "/nix/store/../etc", "/nix/store/a/b", "plotly"])
def test_module_fetch_rejects_non_store_directories(client, identity):
    with pytest.raises(RpcFailure):
        client.call("ModuleReadRequest", module=identity)
