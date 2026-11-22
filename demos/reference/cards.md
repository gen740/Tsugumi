# Writing Cards

Stored templates are listed by template_list. Copy one with tools family templates action copy, supplying the exact store and name. For custom Cards, use project_edit to create definitions, schemas, sources and wiring together.

Read this guide with `demo_read reference/cards.md`. The [SDK guides and starters](https://github.com/gen740/tsugumi-kit/tree/main) cover each language.

## Implementing a Draft

One project_edit can replace a Draft's definition, add its sources and rebind its wire ends atomically. Existing
_meta.json description, content and context stay as written. The host requires a nonempty description, and context
when the original Draft has a description or content; an empty field can be filled with a guarded metadata edit.
Context records what the person asked and what the Draft says. fill_context is a project_save convenience;
project_edit writes the context field itself. Boundary endpoints in tsugumi.json are editable in the same way.

For example, plan starts with _card.json {"type":"draft"} and _meta.json
{"description":"Make a greeting","content":"Return hello."}, with no context or source files. Its incoming wire is
start:out -> plan:"", and its outgoing wire is plan:"" -> next:"" (next remains a Draft). An end on a Card that stays a Draft keeps port "": only the implemented Card's own ends get real ports. These project_edit
arguments implement plan and retain its intent text and every unrelated project field:

```json
{
  "edits": [
    {
      "file_path": "definitions/plan/_card.json",
      "selector": [],
      "old_value": {
        "present": true,
        "value": {
          "type": "draft"
        }
      },
      "new_value": {
        "present": true,
        "value": {
          "type": "code",
          "inputs": [
            {
              "name": "start"
            }
          ]
        }
      }
    },
    {
      "file_path": "definitions/plan/_meta.json",
      "selector": [
        {
          "field": "context"
        }
      ],
      "old_value": {
        "present": false
      },
      "new_value": {
        "present": true,
        "value": "The Draft asks for a greeting."
      }
    },
    {
      "file_path": "definitions/plan/_outputs.fbs",
      "put": "namespace greeting;\ntable Greeting { value:string; }\ntable Outputs { result:Greeting (required); }\nroot_type Outputs;\n",
      "expect_missing": true
    },
    {
      "file_path": "definitions/plan/default.nix",
      "put": "{ pkgs, params, inputs, schemas }: let sdk = inputs.tsugumi.pythonSdk; in { run = sdk.pythonCard { inherit schemas; src = ./src; }; }\n",
      "expect_missing": true
    },
    {
      "file_path": "definitions/plan/src/main.py",
      "put": "from card import Output\nfrom outputs.result.greeting.Greeting import GreetingT\n\ndef run(input):\n    return Output(result=GreetingT(value=\"hello\"))\n",
      "expect_missing": true
    },
    {
      "file_path": "tsugumi.json",
      "selector": [
        {
          "field": "canvas"
        },
        {
          "field": "connections"
        },
        {
          "match": {
            "from": {
              "card": "start",
              "port": "out"
            },
            "to": {
              "card": "plan",
              "port": ""
            }
          }
        },
        {
          "field": "to"
        },
        {
          "field": "port"
        }
      ],
      "old_value": {
        "present": true,
        "value": ""
      },
      "new_value": {
        "present": true,
        "value": "start"
      }
    },
    {
      "file_path": "tsugumi.json",
      "selector": [
        {
          "field": "canvas"
        },
        {
          "field": "connections"
        },
        {
          "match": {
            "from": {
              "card": "plan",
              "port": ""
            },
            "to": {
              "card": "next",
              "port": ""
            }
          }
        },
        {
          "field": "from"
        },
        {
          "field": "port"
        }
      ],
      "old_value": {
        "present": true,
        "value": ""
      },
      "new_value": {
        "present": true,
        "value": "result"
      }
    }
  ]
}
```

The returned diagnostics and project_validate check the saved graph; run_record checks what a run produced.

## Code Cards

A Code Card has a `_card.json` with `"type": "code"`, an `_outputs.fbs` catalog,
a `default.nix`, and sources. Declare each input by name, for example
`"inputs": [{"name": "config"}]`. Connect one wire per input. The type comes
from the upstream output. Use a Merge Card to join several sources. A Code
Card waits for its connected inputs, and its SDK reports `input_missing`
with the port name when the code requires an absent input.

Each output is a field in the root table of `_outputs.fbs`. A FlatBuffer
port uses a table type, and a file or folder uses
`path:string (tsugumi: "path")` with `attribute "tsugumi";`. Return the path of
a file or folder created under the working directory, or an input Path
unchanged to forward it. An Input Card can publish a saved project file or
folder. Its string `value` parameter names the path relative to the Card's
definition, or uses `project:<path>` relative to its project root.

The core generates `_inputs.fbs`, `_outputs.fbs`, and `_params.fbs` from the
saved definition and current wiring. Pass the supplied `schemas` directory
to the language builder as in the Draft example. `run` names the built Card
or an ordered list of implementations. Declare `views` alongside `run` to
display live values, and expose Nix values as `exports` for connected Cards.
The SDK handles its payloads. The core passes them through and handles the
envelope, graph, and filesystem boundaries.

Call `tools` with family `sdk` and action `api` after saving the Card to see
the generated sources and README under `share/tsugumi/api/`. Use those
types when writing the Card. The [SDK guides](https://github.com/gen740/tsugumi-kit/tree/main/docs)
cover language entry points, input lookup, params, and view effects.
Each call starts a fresh worker by default. Set `reuseWorker: true` when
the Card intentionally carries process state across calls.

A Card declares secrets by name in `_card.json`, for example
`"secrets": ["database_password"]`. The user sets the value in project
settings. The worker exposes it to the Card's process as an environment
variable.

## Additional tools

`tools {"family":"files"}` returns file utilities and their argument schemas;
`tools {"family":"sdk"}` returns the Card API (`api`: the generated code under `share/tsugumi/api/` of the Card's build) and flake resolution schemas.
Supply an action and its arguments to call it, for example:

```json
{"family":"runs","action":"status","arguments":{"run_id":"1","wait_seconds":300}}
```

The files `template` action returns a new Card's `definition_json` and sources.
Save them under `definitions/<id>/` together with its canvas instance in one
`project_edit`. Templates use their catalog ids (`python`, `cpp`, `marimo`, etc.).

The files `shell` action runs in the project's environment after a separate Shell
grant. Card paths are project-relative. Attached folders are read-only at
`attached/<first 16 hex digits of SHA-256 of the folder path>/`; `scratch/` persists
for the conversation. Successful source edits save one validated revision. Replies
include bounded stdout/stderr and paths to the full output.

`project_copy` copies attached, Card or conversation scratch files into Card source
paths and returns their sizes and hashes. Data can also stay attached: a Card's
Add the folder under `grants.hostPaths` and read the path from the named `TSUGUMI_GRANT_<NAME>` variable during execution.

The SDK `resolve_source` action resolves a flake for `nixSources`. To customize its
packages, `nixSources[].overrides` accepts JSON maps such as
`{"packageName":{"override":{"argument":"value"},"overrideAttrs":{"pname":"custom"}}}`.
A value `{"fetchurl":{"url":"https://...","sha256":"..."}}` fetches pinned source
or patches. Cards consume `inputs.sources.<name>.packages.<packageName>` with the
system already selected. Saving broader permissions or changed overrides requests
the person's approval.
