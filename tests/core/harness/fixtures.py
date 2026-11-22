"""Prepare external workers; save their card definitions through public RPC."""

import json

from harness.scenario import Project, STRING_OUTPUT


def mock_plan(runs, *, views=(), execution_list=False, **options):
    return {"product": {"runs": runs, "views": list(views)}, "executionList": execution_list, **options}


def add_worker(graph, name, package, actions, *, outputs=STRING_OUTPUT, views=(), **settings):
    """Add a card whose external worker follows the supplied scenario."""
    package.mkdir(parents=True, exist_ok=True)
    (package / "scenario.json").write_text(json.dumps({"actions": actions}))
    return graph.card(
        name, {"type": "code", "sandbox": {"filesystem": False}, "recordIO": True, **settings},
        outputs=outputs, plan=mock_plan([str(package)], views=views),
    )


def worker_project(client, project, directory, actions, *, outputs=STRING_OUTPUT, **settings):
    """Save a project containing one scripted worker card named work."""
    add_worker(Project(project), "work", directory / "package", actions,
               outputs=outputs, **settings).save(client)
