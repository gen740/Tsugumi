# Handoff: moving the lead session to Claude cloud (2026-11-25)

Until now the lead session ran on the owner's Mac. It coordinated Codex runs and Claude sub-agents, and it merged green PRs. From now on the main development environment is a Claude cloud session, because the Mac is busy with other work. This file is what the cloud session needs to continue, and it is written for that session.

## Read these first

| File | What it holds |
| --- | --- |
| [docs/Philosophy.md](../Philosophy.md) | Why the project exists. Every design argument comes back to it. |
| [decision-log.md](decision-log.md) | The owner's decisions from 2026-11-23 to 2026-11-25: project layout, nixPath, Card definition, Card ABI, artifacts, graph semantics, display, project files, pack. A later entry overrides an earlier one. |
| [views-and-capabilities.md](views-and-capabilities.md) | The final view contract: uuid capabilities, `view_open` and routes, the dawn and html view ABIs, time and sound, input, windows, `view_close`, and addressing messages by Card path (section 16). |
| [glowroot-design.md](glowroot-design.md) | The Glowroot game demo: screens as Cards, the skill tree, rebirth, sound. It is the acceptance test for views and pack. |
| [docs/CardABI.md](../CardABI.md), [docs/CoreAndSDK.md](../CoreAndSDK.md), [docs/SandboxContract.md](../SandboxContract.md) | The implemented contracts. If code and these docs disagree, the decision log is the tie-breaker. If the log does not settle it, ask the owner. |

## Working with the owner

- **Language.** Reply to the owner in Japanese, always, including one-line status notes. Code, comments, commits, PRs and repository docs are in English.
- **The owner's decisions are absolute.** PR and issue text written by agents is only a proposal.
  - Before design work, check the decision log and the owner's recent words.
  - Never write your own proposal down as a decision. When something is unclear, ask.
  - Past mistakes happened exactly here: pack was built against the owner's design, ABI 8 silently reversed a rule, and view modules were put in the UI.
- **Ask only what is the owner's call.** Answer derivable questions yourself. When a message ends with "?", answer it. Keep running work going, but do not start new actions as the answer.
- **The owner is a veteran engineer.** Use precise terms and keep identifiers. Give your own opinion when it is asked for. No analogies and no simplification.
- **Before proposing a feature or a guard,** check:
  - the premise;
  - the actual problem;
  - evidence that it happens (examples you made up do not count);
  - whether the guard would really prevent it.

  Drop the proposal if it fails any of these.
- **Measure only when the result would change a decision.** Label guesses as guesses.
- **Merging:**
  - The lead may merge a PR without asking when:
    - CI is fully green;
    - the diff or report has been checked against the owner's decisions;
    - no owner question is open on it.
  - Mark drafts ready first. Report each merge afterwards.
  - Never use `--admin` and never force-push.

## Project rules that are easy to forget

- **No compatibility code before v1.0.0.** Never add migrations, old-format readers or aliases; delete the old thing.
- **Run data:**
  - Only a person deletes, keeps or reuses run data. The tool never does it on its own.
  - The tool may move data aside, but only with the owner's approval.
- **No definitions shared across projects.** Copying and adapting another project's Card is fine.
- **The UI owns presentation:** shapes, ports, wire routes and anything else computed from layout. Only semantics move to hostd or tsugumid.
- **Agent prompts:**
  - Describe capabilities briefly and accurately.
  - Avoid prohibitions and lists of cases.
  - Tests do not pin prompt wording.
- **What counts as tested:**
  - `nix build .#ci-core` only builds.
  - Name the suites that actually ran.
  - `tests/check-fast.sh` runs before every push.
  - Some suites run only on Linux (`tests/core/integration`, the real remote worker).
- **Secrets:**
  - Never read or print credentials.
  - API keys live only in hostd's store, never in the launcher's environment.
- **Security controls:**
  - Weakening a security control needs the owner's explicit approval.
  - Never get around a permission that was denied to a sub-agent.

## How work is run

- **Model allocation.** Take the first free slot, in this order:
  1. **PRO:** the `claude` CLI on Pro, at most 1 running.
     - It is blocked until 2026-11-27 11:00 by the weekly limit.
     - The Pro sign-in expires between 11/27 and 11/30. Remind the owner to run `claude auth login`.
  2. **SOL:** `codex exec -m gpt-6.2-sol`, at most 5 running.
     - It does heavy implementation.
     - `gpt-6.2-astra` does audits. Ask the owner before using Astra; they set a time cut-off for it.
  3. **MAX:** sub-agents of the lead session.
- **When reporting an assignment,** give only the slot label (PRO, SOL or MAX).
- **Parallel runs:**
  - Each run gets its own worktree or clone, with explicit file ownership.
  - Each run gets a self-contained brief that quotes the owner's decisions verbatim.
  - Each brief ends with:
    - English commits ending in `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`;
    - `tests/check-fast.sh` before each push and green CI;
    - no compatibility code;
    - "do not merge" (the lead merges);
    - stop only processes you started, by pid, never by name.
- **The lead's role:** plan, then interrogate the plan and the reports. Read only as much code as you need to judge a report. Bring the owner conclusions you can defend.
- **Codex gotchas:**
  - Close stdin (`< /dev/null`), or `codex exec` hangs.
  - Runs that push need workspace-write with network access.
  - OpenAI's cybersecurity filter sometimes aborts a run (for example on fake-host or crash tests). Hand that work to Claude.
- **The local dispatcher scripts (`dispatch.sh`, `queue.sh`, `heavy.sh`, `health.sh`) stayed on the Mac.** Rebuild what is needed in the cloud. The pattern:
  - a dispatcher that counts the real worker processes per slot;
  - a done-file that the waiters poll;
  - a health check every 30 minutes that acts on real problems and is otherwise quiet.
- **tsugumi-kit:**
  - Language SDKs and templates live in [tsugumi-kit](https://github.com/gen740/tsugumi-kit).
  - A core change that needs a kit change makes a kit branch. The core then re-pins `flake.lock`, the templates and the demo locks to that branch commit.
  - Never force-push kit either.

## In flight at the time of handoff

These runs were still going on the Mac when this file was written. The local lead session waits for them, checks them and merges what is green, and then hands over.

| Work | Where | State |
| --- | --- | --- |
| **Native tsugumi-pack.** Cards run as native libraries; default mode (a resident process per Card, shared-memory results) and fast mode (one process); debug exports; `result_alloc`; borrowed input array | draft [gen740/Tsugumi#456](https://github.com/gen740/Tsugumi/pull/456), `feat/pack-native-app`; may become a stack (ABI changes first) | running |
| **Seatbelt DNS.** For network-granted Cards under ipc isolation, allow only the unix socket `/private/var/run/mDNSResponder`, and restore gw150914's `ipc` option | branch from main, PR to come | running |
| **Host state format versions.** Say clearly when state was written by another version, and let a person choose to move it aside | [gen740/Tsugumi#458](https://github.com/gen740/Tsugumi/pull/458), `fix/state-format-version` | running |
| **View messages addressed by Card path.** `tsugumi_view_send(to, …)` / `tsugumi.send(bytes, to)`; the all-zero opener route is removed; Glowroot updated (views-and-capabilities section 16) | `feat/view-send-by-path`, PR "Address view messages by Card path" to come | running |

When one of these lands, check that the PR's text matches the owner's decisions above. If a run left questions, they are in its PR body.

## What needs the Mac

The cloud has no macOS. GitHub Actions runs the macOS job, so CI is the verifier there. The following can only be checked on the owner's Mac, by a local session:

- **The Mac app.**
  - Build: `TSUGUMI_CODESIGN_IDENTITY="Apple Development" ui/macos/build-app.sh`.
  - Launch: only `ui/macos/tools/run-ui.sh` or `open -g`. Never take the foreground and never show dialogs; someone is using the Mac.
- **The dev app and its project.** The dev app is `~/Desktop/Tsugumi Dev.app`, with its own support directory; the Glowroot copy is `~/Desktop/glowroot`. Never touch `/Applications/Tsugumi.app` or its VM and state.
- **Seatbelt, view hosting (Dawn, wasmtime, audio) and the Virtualization.framework shared-VM suites.**
  - Apple's virtiofs refuses to traverse execute-only directories. The artifact design was changed because of this (decision log, section E).
- **Native pack acceptance.**
  - Export Glowroot and the other demos as `.app`s.
  - Play them through the debug endpoint.
  - Start the release app with no Nix and no Tsugumi on PATH.
- **The Linux job through `tests/ci-local.sh linux`** (OrbStack).
- **Guest images.** Never build one locally. Use `nix run .#guest-image`, which takes the CI artifact.
- **Sound.** Glowroot and the packed apps play through the Mac's speakers. Keep interactive checks short, use `--mute-audio` for headless browsers, and stop everything you started.

## What comes next (owner's direction)

- **Finish and merge the four in-flight items.**
- **Then the native pack acceptance on the Mac:** Glowroot as release and debug, in both modes, played end to end, plus report and point-cloud.
- **Open questions** are listed under "Open" in the decision log and in each PR's questions. Ask the owner about them; do not decide them.
