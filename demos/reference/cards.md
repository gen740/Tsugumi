# Writing Cards

Stored templates are listed by template_list. Copy one with tools family templates action copy, supplying the exact store and name. For custom Cards, use project_edit to create definitions, schemas, sources and wiring together.

Read this guide with `demo_read reference/cards.md`. The [SDK guides and starters](https://github.com/gen740/tsugumi-kit/tree/main) cover each language.

## Implementing a Draft

One project_edit can replace a Draft's definition, add its sources and rebind its wire ends atomically. Existing
README.md description and body stay as written. Add a one-line description if it is empty.
Replace the README with a guarded whole-file text edit when it needs a description. Boundary
entries in tsugumi.json have `name`, `card` and `port` fields.

For example, plan starts with _card.json {"type":"draft"} and a README.md with
`description: Make a greeting` in the front matter and `Return hello.` in its body. Its incoming wire is
start:out -> plan:"", and its outgoing wire is plan:"" -> next:"" (next remains a Draft). An end on a Card that stays a Draft keeps port "": only the implemented Card's own ends get real ports. These project_edit
arguments implement plan and retain its intent text and every unrelated project field.

The project declares `nixpkgs` and `tsugumi-kit` in `tsugumi.json` under `nixPath` as `{name, url}` entries. With approval, `nix_path_lock_update` writes one name or all names to `tsugumi.lock`, whose entries list stores each name, declared URL, optional revision and NAR hash. Each external uses its own declarations and lock. If an import names an undeclared source, add the name to the owning project's `nixPath` and lock it.

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
      "file_path": "definitions/plan/_outputs.fbs",
      "put": "namespace greeting;\ntable Greeting { value:string; }\ntable Outputs { result:Greeting (required); }\nroot_type Outputs;\n",
      "expect_missing": true
    },
    {
      "file_path": "definitions/plan/default.nix",
      "put": "{ params, inputs }:\nlet\n  pkgs = import <nixpkgs> { };\n  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };\n  common = { inherit params; inputs = ./_inputs.fbs; outputs = ./_outputs.fbs; };\nin { run = sdk.pythonCard (common // { src = ./src; }); api = sdk.pythonApi common; }\n",
      "expect_missing": true
    },
    {
      "file_path": "definitions/plan/src/main.py",
      "put": "from card import Output\nfrom greeting.Greeting import GreetingT\n\ndef run(input):\n    return Output(result=GreetingT(value=\"hello\"))\n",
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

Declare input names in _card.json and add one wire per input. Use Merge when joining sources.
Each output is a table field in the root of _outputs.fbs. Mark outputs every call must emit with
(required). Omitted outputs can cause downstream Cards to skip. Tsugumi generates _inputs.fbs
from wiring and upstream schemas. Pass that schema to the SDK builder.

Edit values in params.nix, a flat literal attrset such as `{ seed = 7; tariff = "spot"; }`.
A Card without params has no params.nix.
The SDK embeds the evaluated params at build time. In `default.nix { params, inputs }`,
`inputs.<card_id>` is the upstream Card's Nix exports. See the [Card guide](https://github.com/gen740/Tsugumi/blob/main/docs/CardGuide.md)
and the [SDK contract](https://github.com/gen740/tsugumi-kit/blob/main/docs/sdk-contract.md).

Use `include "tsugumi/artifact.fbs";` and `tsugumi.Artifact` for files or directories. Create or upload
artifacts through the SDK. Open granted inputs via the SDK for a read-only directory.

Return `run` as a derivation or a list of `{ label; run; }`. Declare live views through `views`.
Use `evaluate_nix` to evaluate in the saved Card's directory on the local system with locked Nix
imports, `params` and `inputs` in scope. Set `build: true` to realise a derivation and read its
bounded output. Evaluate `(import ./. { inherit params inputs; }).api` with that option to read
the generated SDK files in `share/tsugumi/api/`.

Each call starts a fresh worker by default. Set `reuseWorker: true` when
the Card intentionally carries process state across calls.

A Card declares secrets by name in `_card.json`, for example
`"secrets": ["database_password"]`. The user sets the value in project
settings. The worker exposes it to the Card's process as an environment
variable.

## Additional tools

`tools {"family":"files"}` returns file utilities and their argument schemas;
`tools {"family":"sdk"}` returns the Nix expression evaluation and output-schema tools.
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
paths and returns their sizes and hashes. Data can also stay attached. Add the folder under `grants.hostPaths` and read the path from the named `TSUGUMI_GRANT_<NAME>` variable during execution.
