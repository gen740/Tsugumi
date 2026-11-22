"""Render real R Markdown/Quarto reports through the public Card contract."""
import json
import os
import subprocess
import uuid

import pytest
from test_language_cards import project  # pytest fixture

from test_language_cards import NUMBERS, write_project
from harness.observations import read_pages
from harness.file_events import wait_for_exit
from projects import run_and_wait, start_run, wait_for_log, wait_run
from tsugumi_client import new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState

OUTPUTS = '''attribute "tsugumi";
namespace report;
table Path { value:string; }
table Outputs { report:Path (required, tsugumi: "directory"); html:Path (required, tsugumi: "file"); }
root_type Outputs;
'''
REPORT = '''---
title: Report test
params:
  tsugumi: null
---

```{r check-input}
stopifnot(params$tsugumi$inputs$data$big == "9007199254740993")
values <- params$tsugumi$inputs$data$values
knitr::kable(data.frame(value = values))
```

Total: `r sum(values) * params$tsugumi$params$scale`

```{r figure}
plot(values, type="b")
```

![Relative source asset](badge.svg)
'''


def report_project(root, extension, source=REPORT):
    write_project(root, {
        'data': {'card': {'type': 'code', 'inputs': [{'name': 'start'}]}, 'outputs': NUMBERS,
                 'nix': '''{ pkgs, inputs, params }: { run = ''
                   printf '%s' '{"numbers":{"values":[1,2,3],"big":9007199254740993}}' > "$TSUGUMI_OUTPUT"
                 ''; }'''},
        'report': {'card': {'type': 'code', 'inputs': [{'name': 'data'}],
                           'recordIO': True, 'retainArtifact': True, 'artifactOutput': 'report',
                           'params': [{'name': 'scale', 'type': 'double', 'data': 2}]},
                   'outputs': OUTPUTS,
                   'nix': '{ pkgs, inputs, params }: { run = inputs.tsugumi.reportCard { src = ./src; entry = "report.' + extension + '"; }; }',
                   'src': {'report.' + extension: source,
                           'badge.svg': '<svg xmlns="http://www.w3.org/2000/svg" width="80" height="24"><rect width="80" height="24" fill="green"/></svg>'}},
        'read': {'card': {'type': 'code', 'inputs': [{'name': 'report'}]},
                 'nix': '''{ pkgs, inputs, params }: { run = ''
                   test -s "$DIR_REPORT/index.html"
                 ''; }'''},
    }, [('start', 'out', 'data', 'start'), ('data', 'numbers', 'report', 'data'), ('report', 'report', 'read', 'report')])


@pytest.mark.parametrize('extension', ['Rmd', 'qmd'])
def test_report_html_assets_and_typed_downstream(project, extension):
    root, connect = project
    source = REPORT
    if extension == 'Rmd':
        source = REPORT.replace('{r figure}', '{r figure, cache=TRUE}') + """
```{r file-inputs}
stopifnot(readLines(params$tsugumi$paths$file) == "file-data")
stopifnot(readLines(file.path(params$tsugumi$paths$folder, "data.txt")) == "file-data")
```
"""
    report_project(root, extension, source)
    if extension == 'Rmd':
        wire_file_inputs(root)
    client = connect()
    run, state = run_and_wait(client, 'start')
    assert state.state == RunState.succeeded, state.failure and text(state.failure.diagnostic)
    assert client.call('RunRecordRequest', runId=run, card=path('read')).status.state == CardState.succeeded
    artifact = report_artifact_id(client, run)
    rendered = read_pages(client, 'ArtifactReadRequest', artifactId=artifact, member='index.html', page_bytes=262144)
    assert b'Total: 12' in rendered
    assert b'data:image/png;base64,' in rendered and b'data:image/svg+xml' in rendered
    metadata = json.loads(read_pages(client, 'ArtifactReadRequest', artifactId=artifact,
                                    member='metadata.json', page_bytes=262144))
    assert metadata == {'source': 'report.' + extension}
    if extension == 'Rmd':
        check_report_after_edit(root, client, artifact, rendered)


def report_artifact_id(client, run):
    artifact = next(a for a in client.call('ArtifactListRequest', runId=run).artifacts if text(a.output) == 'report')
    return text(artifact.artifactId)


def wire_file_inputs(root):
    definition = root / 'definitions/data'
    (definition / '_outputs.fbs').write_text(NUMBERS.replace('table Outputs {', '''table Path { value:string; }
table Outputs { file:Path (required, tsugumi:"file"); folder:Path (required, tsugumi:"directory");'''))
    (definition / 'default.nix').write_text('''{ pkgs, inputs, params }: { run = ''
      mkdir -p "$TSUGUMI_OUTPUT_DIR/files"
      printf '%s\\n' file-data > "$TSUGUMI_OUTPUT_DIR/files/data.txt"
      jq -n --arg f "$TSUGUMI_OUTPUT_DIR/files/data.txt" --arg d "$TSUGUMI_OUTPUT_DIR/files" \\
        '{numbers:{values:[1,2,3],big:9007199254740993},file:{value:$f},folder:{value:$d}}' > "$TSUGUMI_OUTPUT"
    ''; }''')
    report = root / 'definitions/report/_card.json'
    card = json.loads(report.read_text())
    card['inputs'] += [{'name': 'file'}, {'name': 'folder'}]
    report.write_text(json.dumps(card))
    doc = root / 'tsugumi.json'
    project_doc = json.loads(doc.read_text())
    project_doc['canvas']['connections'] += [{'from': {'card': 'data', 'port': port}, 'to': {'card': 'report', 'port': port}} for port in ['file', 'folder']]
    doc.write_text(json.dumps(project_doc))

def check_report_after_edit(root, client, original, rendered):
    card_file = root / 'definitions/report/_card.json'
    card = json.loads(card_file.read_text())
    card['params'][0]['data'] = 3
    card_file.write_text(json.dumps(card))
    source_file = root / 'definitions/report/src/report.Rmd'
    source_file.write_text(source_file.read_text().replace('Total:', 'Revised result:'))
    upstream = root / 'definitions/data/default.nix'
    upstream.write_text(upstream.read_text().replace('[1,2,3]', '[3,4,5]'))
    dependency = root / 'definitions/report/default.nix'
    dependency.write_text(dependency.read_text().replace('src = ./src;', 'src = ./src; packages = ps: [ ps.digest ];'))
    reload(client)
    run, state = run_and_wait(client, 'start')
    assert state.state == RunState.succeeded, state.failure and text(state.failure.diagnostic)
    updated = report_artifact_id(client, run)
    assert b'Revised result: 36' in read_pages(client, 'ArtifactReadRequest', artifactId=updated,
                                               member='index.html', page_bytes=262144)
    assert read_pages(client, 'ArtifactReadRequest', artifactId=original,
                      member='index.html', page_bytes=262144) == rendered


def test_report_chunk_failure_stops_downstream(project):
    extension = 'Rmd'
    root, connect = project
    report_project(root, extension, REPORT.replace('plot(values, type="b")', 'stop("report-cell-error")'))
    client = connect()
    run, state = run_and_wait(client, 'start')
    assert state.state == RunState.failed
    assert client.call('RunRecordRequest', runId=run, card=path('report')).status.state == CardState.failed
    assert client.call('RunRecordRequest', runId=run, card=path('read')).status.state == CardState.blocked
    assert not client.call('ArtifactListRequest', runId=run).artifacts


def test_nested_quarto_project_output_directory(project):
    root, connect = project
    report_project(root, 'qmd')
    source = root / 'definitions/report/src'
    (source / '_quarto.yml').write_text('''project:
  type: default
  output-dir: _site
execute:
  freeze: true
''')
    (source / 'nested').mkdir()
    for name in ('report.qmd', 'badge.svg'):
        (source / name).rename(source / 'nested' / name)
    definition = root / 'definitions/report/default.nix'
    definition.write_text(definition.read_text().replace('report.qmd', 'nested/report.qmd'))
    downstream = root / 'definitions/read/default.nix'
    downstream.write_text(downstream.read_text().replace('/index.html', '/nested/index.html'))
    freeze = source / 'nested/_freeze'
    freeze.mkdir()
    (freeze / 'old-result.json').write_text('{"stale": true}')
    client = connect()
    for result in (12, 24):
        run, state = run_and_wait(client, 'start')
        assert state.state == RunState.succeeded, text(client.call('LogRangeRequest', runId=run, card=path('report'), stream=2, limitBytes=65536).text)
        artifact = next(a for a in client.call('ArtifactListRequest', runId=run).artifacts if text(a.output) == 'report')
        members = [text(m.path) for m in artifact.members]
        assert not any(name.endswith('old-result.json') for name in members)
        page = read_pages(client, 'ArtifactReadRequest', artifactId=text(artifact.artifactId),
                          member='nested/index.html', page_bytes=262144)
        assert f'Total: {result}'.encode() in page
        if result == 12:
            upstream = root / 'definitions/data/default.nix'
            upstream.write_text(upstream.read_text().replace('[1,2,3]', '[2,4,6]'))
            reload(client)


def reload(client):
    client.call('ProjectReloadRequest', baseRev=client.call('ProjectInspectRequest').rev, opId=new_id(), origin='test')


def test_report_cancel_reaps_renderer_children(project):
    extension = 'Rmd'
    root, connect = project
    marker = f'tsugumi-report-child-{uuid.uuid4().hex}'
    slow = REPORT.replace('plot(values, type="b")',
                          f'system2("bash", c("-c", shQuote("echo report-sleeping >&2; while :; do sleep 60; done"), "{marker}"), wait = FALSE)\n'
                          'Sys.sleep(60)')

    def child_pid():
        output = subprocess.check_output(['ps', '-axww', '-o', 'pid=,command='], text=True)
        return next(int(line.split(None, 1)[0]) for line in output.splitlines() if marker in line)

    report_project(root, extension, slow)
    client = connect()
    run = start_run(client, 'start')
    wait_for_log(client, run, 'report', 2, lambda log: 'report-sleeping' in text(log.text or b""))
    pid = child_pid() if os.environ.get('TSUGUMI_TEST_SHARED_VM') != '1' else None
    client.call('RunCancelRequest', runId=run)
    assert wait_run(client, run).state == RunState.cancelled
    assert not client.call('ArtifactListRequest', runId=run).artifacts
    if pid is not None:
        wait_for_exit(pid)
