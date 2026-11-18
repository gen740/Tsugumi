# tsugumi-pack: a Project as a standalone Mac app

```sh
tsugumi-pack ./CellCount --output ./CellCount.app
./CellCount.app/Contents/MacOS/CellCount --set generate.seed=11 --output ./counts
./WordCount.app/Contents/MacOS/WordCount --file text=./my-notes.txt
```

`--help` on an app lists its parameters and file inputs. Opened from the
Finder, an app runs with the exported values and shows its results folder
(in Documents) when done.

The exporter turns a Tsugumi Project into an app that the recipient runs
offline on an Apple Silicon Mac, with no Tsugumi or Nix installed
([issue #322](https://github.com/gen740/Tsugumi/issues/322)). It runs from a
shell with no hostd; the UI export action and hostd's relay of its progress
come later.

## What it does

1. **Stage the Project.** Its files are copied without `.tsugumi/` (run
   history, versions) or `.git`: what a run produced stays with the author.
2. **Build every Card in the pack guest.** `tsugumi-pack` boots the pack guest
   (below) in *export* mode, with the network on and a store disk that is kept
   between exports (`~/Library/Caches/tsugumi-pack/store.img`, sparse).
   There `tsugumid --prepare-cards` opens the Project and, for every Code
   Card, performs exactly the evaluation and build a run of that Card would
   perform on the local service (`NixRunner::build`), whether or not a run
   would reach it. A Card that does not build fails the export and is named.
3. **Write the payload.** Each Card's runtime closure, its derivations, and
   the nixpkgs sources the Cards are evaluated with, minus what the base image
   already holds, go into `payload.erofs` with their Nix registration and the
   staged Project.
4. **Write the app.** `Contents/MacOS/<Name>` is a copy of the signed
   `tsugumi-pack` executable; `Contents/Resources` holds `vmlinux`,
   `rootfs.erofs` (the pack guest), `payload.erofs` and `pack.json` (the
   triggers and each Card's parameters, with those that change the build
   marked fixed).

The app, when run, boots the same guest in *run* mode with **no network
device**. The payload's store is laid over the image's (overlayfs, writes to
a sparse scratch disk the app removes afterwards: tsugumid keeps run scratch
on a disk), the recipient's parameter values and files are written into a
copy of the Project, and `tsugumid` runs the Workflow from its trigger
through the same RPC the apps use. Every Card's outputs (raw FlatBuffers
bytes, JSON, and the content of File and Directory values), display (PNG,
text, table) and logs are written under the results folder. `summary.json`
lists every invocation and, under `results`, the outputs no other Card
consumes: the Workflow's results. The guest then powers off; Ctrl-C stops it
and removes the run's temporary folder and scratch disk.

## Choices and why

- **The run is an ordinary tsugumid run.** Scheduling, workers, sandboxing,
  batches, branches and failures are tsugumid's own; nothing in `pack/`
  schedules Cards. The price is that the guest carries Nix and the nixpkgs
  sources, and every run evaluates its Cards again (offline, finding every
  derivation and output already present). A tsugumid that could accept its
  build plans as data would remove that evaluation; it is not needed for the
  outcome and is left for a measured follow-up.
- **`--prepare-cards` in tsugumid instead of a build in `pack/`.** Which files
  a Card's evaluation sees (generated contracts, parameters, upstream exports,
  sources) is tsugumid's logic. Duplicating it in the exporter would let the
  app's builds drift from the Workflow's; one flag reuses it, and also covers
  branches a run would not take, which a recorded run cannot.
- **A guest, not `nix bundle`.** The Cards are Linux programs with absolute
  `/nix/store` paths. A minimal Linux guest with the closure placed directly
  in a read-only image runs them unmodified; bundling and unpacking them again
  on macOS would need a writable `/nix` there, which a recipient's Mac does
  not have.
- **One guest image for export and run.** The exporter needs a Linux Nix
  with network to build; the app needs the same core offline. One image with
  two modes keeps the builder and the runner identical, and makes
  `tsugumi-pack` need nothing but Virtualization.framework on the author's
  Mac. The kernel is the shared VM's (`vm/guest/kernel.nix`).
- **One executable for exporter and launcher.** It is the only program that
  needs the virtualization entitlement; inside an app it finds `pack.json`
  and becomes that app's launcher. The launcher only boots the guest, hands
  over `request.json` and moves the results folder out. Viewing is left to
  the files it writes (an image viewer, a text editor): there is no viewer in
  this path.
- **Parameters, not rebuilt definitions.** The recipient can change the
  parameters of Code, input and switch Cards (`--set card.param=value`,
  `--params file.json`; `--help` lists them). Parameters marked `nix` change
  the Card's build, and a fork's or branch's change the graph, so they are
  fixed at export; the app refuses them rather than attempting a build offline.
- **Files through the input Card.** `--file CARD=PATH` replaces the file of
  an input Card whose output is a File. The core reads an input Card's file
  only from the Card's own folder, so the app copies the recipient's file
  into the copy of the Project there (`recipient/<name>`) and points the Card
  at it. An input Card is not built, so this changes no build.
- **Values are recorded in the app.** Record I/O is off by default, which
  would leave the recipient with displays only. The app's copy turns it on
  for every Code Card; it is a setting, outside what a build evaluates.
- **Size.** The nixpkgs sources are 130 MiB of every payload (191 MiB
  unpacked) and, with the derivations, are there only for the run's
  evaluation: a one-Card Python Project's payload is 223 MiB, cell-count's
  544 MiB. Removing that evaluation (above) would remove them.

## Layout

| Path | What |
| --- | --- |
| [guest/default.nix](guest/default.nix) | The pack guest (`packages.aarch64-linux.pack-guest`): `/init` for both modes |
| [guest/export.sh](guest/export.sh) | Export mode: build every Card, write `payload.erofs` |
| [guest/run.py](guest/run.py) | Run mode: apply the request, run from the trigger, write the results |
| [launcher/](launcher/) | `tsugumi-pack` (Swift): exporter and app launcher |
| [build.sh](build.sh) | Builds `dist/tsugumi-pack` (the signed command and the guest) |

## Building

```sh
TSUGUMI_CODESIGN_IDENTITY="Apple Development" TSUGUMI_PACK_LINUX="orb -m nixos" pack/build.sh
dist/tsugumi-pack/bin/tsugumi-pack demos/en/cell-count --output CellCount.app
```

The guest needs an aarch64-linux Nix builder (`TSUGUMI_PACK_LINUX` names a
command that runs a Linux shell seeing this checkout at the same path, such
as an OrbStack NixOS machine; otherwise `nix build` with a configured Linux
builder). `TSUGUMI_PACK_GUEST=DIR` uses a guest built elsewhere.

## First end-to-end check

`demos/en/cell-count` (seven Python and Bash Cards, two forks), exported and
run on an M-series Mac (14 CPUs; the guest gets 8 GiB), 2026-11-18:

| | |
| --- | --- |
| Export, store disk warm / first export | 58 s / 77 s (builds: 5 s / 28 s, the rest is writing the payload) |
| App size | 728 MiB on disk: payload 544 MiB (4,804 store paths), guest image 170 MiB, kernel 14 MiB, launcher 0.25 MiB |
| Download (`ditto -c -k` zip) | 466 MiB |
| Boot to tsugumid ready | 1.9 s |
| First Code Card result (`mitosis`) / first figure (`generate`) | 2.2–2.6 s / 3.6 s after launch |
| Whole Workflow, results written | 23.8–24.8 s (the parameter sweep is 18 s of it) |

**Offline.** The app's guest has no network device at all, so nothing in it
can reach the network; the run still evaluated and started every Card, which
shows the payload is complete. (The Mac's own network was left on: this Mac is
shared, and the guest's isolation does not depend on it.)

**Same results as the Workflow in Tsugumi.** The reference is the same
Project run by this branch's `tsugumid` directly on an online aarch64 NixOS
host (OrbStack), with its own store and the normal project location. All six
figures (PNG) and all seven Card logs are byte-identical to the app's. With a
new input, `--set generate.seed=11 --set generate.cells=120 --set
real.min_area=20`, the counts change (score: 96 by threshold, 117 by
watershed) and are again byte-identical to the reference given the same
values. Setting a `nix` parameter (`mitosis.hash`) or a fork's `branches` is
refused with its reason.

**A recipient's file.** A three-Card Project (trigger, an input Card with a
text file, a Python Card that counts lines and words and writes the text
upper-cased as a File) exported to a 407 MiB app. With `--file
text=mine.txt` the app returned `counts.json` (3 lines, 6 words) and
`upper/upper.txt` for the given file in 1.9 s, identical to the reference
run given the same file (the File value's JSON names the copy, not the
guest's path).

Not checked here: a second Mac without Tsugumi or Nix (nothing on this Mac
is used by the app, but its development signature would need Developer ID
signing to open elsewhere), and opening an app from the Finder (this Mac is
shared, so nothing was brought to the front).

## Not done yet

- How a recipient gives inputs. `--set` and `--file` are provisional: they
  exist to check that an exported app runs on new values. Which values an
  app exposes (likely only input Cards), how folders and batches are passed,
  and the recipient's form follow from how Tsugumi itself takes a Workflow's
  inputs, which is still to be designed; they will be replaced then.
- hostd integration and the UI's export action (the tool's progress lines
  are already one per stage).
- Directory inputs: an input Card reads one file; a recipient's folder
  needs a Card that takes a Directory, which input Cards do not offer today.
- Progress in the Finder: an app opened there shows nothing until its
  results folder opens.
- Distribution signing: an app signed with a development certificate runs on
  this Mac; another Mac needs Developer ID signing and notarization.
- Cards built for x86_64-linux (Rosetta in the guest) and remote execution
  services are not exported.
