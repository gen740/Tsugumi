"""Public history references use project numbers and natural composition."""
from harness.scenario import run, wait_run
from test_rpc_run_history import prepare_history
from tsugumi_client import new_id, path, text
from Tsugumi.Data.RunState import RunState


def test_numbers_never_reuse_deleted_history_and_artifacts_are_composed(core_factory, project, tmp_path):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    prepare_history(client, project, tmp_path)
    assert len(daemon.ready["project_id"]) == 6
    for number in (1, 2):
        run_id = run(client)
        assert run_id == str(number)
        assert wait_run(client, run_id).state == RunState.succeeded
        artifacts = client.call('ArtifactListRequest', runId=run_id, card=path('work')).artifacts
        assert text(artifacts[0].artifactId) == f'{number}/work/1/doc'
        assert text(artifacts[0].name) == 'report.txt'
        # These are exactly the fields selected by run/log/artifact evidence links.
        assert text(client.call('RunStatusRequest', runId=run_id).runId) == run_id
        assert text(client.call('LogRangeRequest', runId=run_id, card=path('work'), invocation=1, stream=1).text) == 'a recorded log\n'
    client.call('RunHistoryDeleteRequest', opId=new_id())
    daemon.stop()
    daemon.start()
    client = daemon.connect()
    run_id = run(client)
    assert run_id == '3'
    assert wait_run(client, run_id).state == RunState.succeeded
