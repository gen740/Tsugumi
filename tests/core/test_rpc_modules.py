"""Module retrieval is an immutable, bounded RPC read."""

import pytest

from tsugumi_client import RpcFailure


def test_module_fetch_rejects_non_store_directories(client):
    for identity in ("/etc", "/nix/store", "/nix/store/../etc", "/nix/store/a/b", "plotly"):
        with pytest.raises(RpcFailure):
            client.call("ModuleReadRequest", module=identity)
