"""Prepare external workers; save their card definitions through public RPC."""

import json

from harness.scenario import Project, STRING_OUTPUT


def add_worker(graph, name, package, actions, *, outputs=STRING_OUTPUT, **settings):
    """Add a card whose external worker follows the supplied scenario."""
    package.mkdir(parents=True)
    (package / "scenario.json").write_text(json.dumps({"actions": actions}))
    return graph.card(
        name, {"language": "bash", "sandbox": False, "recordIO": True, **settings},
        outputs=outputs, manifest={"run": {"package": str(package)}},
    )


def worker_project(client, project, directory, actions, *, outputs=STRING_OUTPUT, **settings):
    """Save a project containing one scripted worker card named work."""
    add_worker(Project(project), "work", directory / "package", actions,
               outputs=outputs, **settings).save(client)
