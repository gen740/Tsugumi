# Recorded environment UI checks

Build the manual fixture with `node web/scripts/environment-browser.mjs /tmp/environment-ui`, then serve that directory with `python3 -m http.server 8765 --bind 127.0.0.1 --directory /tmp/environment-ui`.

The fixture renders the production history components with deterministic RPC responses. It does not exercise daemon persistence; `tests/core/test_run_environments.py` covers that separately.

Checked in the in-app browser on 2026-11-12:

- Invocation view: 205 dependencies appear in pages of 100, 100 and 5. Next is disabled on the third page.
- Search for `package-204` after visiting the last page: the result appears and the page resets to 1.
- Type `slow`, then immediately `package-203`: the delayed old response does not replace the current result.
- Missing environment: the view explicitly says no environment was recorded.
- Enable Fail requests, open Invocation, then disable it and choose Retry: the connection error is shown and the environment can be loaded again.
- Run overview groups `bands` and `spectrum` under one named environment. `trigger` is shown separately as unrecorded.
- Compare against `previous`: bands is unchanged, spectrum changed, trigger unrecorded. Expanding spectrum shows `new-unique` as added and `old-unique` as removed, with full paths available by disclosure.

These checks are manual, not automated browser tests. The fixture is not included in the production application.
