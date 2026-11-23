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
2. **Evaluate and build every Card, once, at export.** `tsugumi-pack` boots
   the export guest (below) with the network on and a store disk that is
   kept between exports (`~/Library/Caches/tsugumi-pack/store.img`, sparse).
   There `tsugumid --prepare-cards` opens the Project and, for every Code
   Card, performs exactly the evaluation and build a run of that Card would
   perform on the local service (`NixRunner::build`), whether or not a run
   would reach it. For each Card it records the evaluation key (the nixpkgs
   pin, the system and the content-addressed definition, contract,
   parameter and upstream folders) and the build: plan, closure and run
   list shape. A Card that does not build fails the export and is named.
3. **Write the payload.** The Cards' runtime closures, minus what the app's
   runner image already holds, go into `payload.erofs` with those records
   (`cards.bin`) and the staged Project. No nixpkgs sources, derivations
   or Nix database: nothing in the app evaluates.
4. **Write the app.** `Contents/MacOS/<Name>` is a copy of the signed
   `tsugumi-pack` executable; `Contents/Resources` holds `vmlinux`,
   `rootfs.erofs` (the runner image), `payload.erofs` and `pack.json` (the
   triggers and each Card's parameters, with those that change the build
   marked fixed).

The app, when run, boots the runner image with **no network device**. The
payload's store is laid over the image's (a read-only overlay), the
recipient's parameter values and files are written into a copy of the
Project on a sparse scratch disk the app removes afterwards, and `tsugumid
--prepared-cards` runs the Workflow from its trigger through the same RPC
the apps use. For each Card it computes the evaluation key as it would
before evaluating, and uses the recorded build only if the key is the
export's; a Card without a record, or one that changed, fails with that
reason instead of being evaluated. Every Card's outputs (raw FlatBuffers
bytes, JSON, and the content of File and Directory values), and logs are written under the results folder. `summary.json`
lists every invocation and, under `results`, the outputs no other Card
consumes: the Workflow's results. The guest then powers off; Ctrl-C stops it
and removes the run's temporary folder and scratch disk.

## Choices and why

- **The run is an ordinary tsugumid run, without evaluation.** Scheduling,
  workers, sandboxing, batches, branches and failures are tsugumid's own;
  nothing in `pack/` schedules Cards. Only the step from a Card's definition
  to its build is replaced: in an app the definitions are fixed, so
  evaluating them again could only give the same builds. The first version
  of this tool did evaluate in the app (offline, with nixpkgs and every
  derivation shipped); dropping that took 156 MiB off cell-count's app and
  the first result from 2.2 s to 0.9 s.
- **`--prepare-cards` and `--prepared-cards` in tsugumid, not a build in
  `pack/`.** Which files a Card's evaluation sees (generated contracts,
  parameters, upstream exports) is tsugumid's logic, and so is the key that
  says two evaluations are the same. The exporter reuses both, so an app's
  builds cannot drift from the Workflow's; it also covers branches a run
  would not take, which a recorded run cannot. Both modes use `/work` as the
  work folder because the key names folders under its `scratch/`.
- **A guest, with the closure placed in an image.** The Cards are Linux
  programs with absolute `/nix/store` paths, so they need a Linux kernel;
  a minimal guest with the closure in a read-only image runs them
  unmodified. `nix bundle`'s default bundler would put the same files in a
  self-extracting archive that unpacks them at every start, and still needs
  a Linux kernel to run them.
- **Two images from one definition.** The export image has Nix, a CA bundle
  and the tools that write the payload; the runner image has tsugumid, its
  run driver and the few tools `/init` uses, and no Nix. Both carry the same
  tsugumid, so the keys computed in both match. `tsugumi-pack` needs nothing
  but Virtualization.framework on the author's Mac. The kernel is the shared
  VM's (`vm/guest/kernel.nix`).
- **One executable for exporter and launcher.** It is the only program that
  needs the virtualization entitlement; inside an app it finds `pack.json`
  and becomes that app's launcher. The launcher only boots the guest, hands
  over `request.json` and moves the results folder out. Viewing is left to
  the files it writes (an image viewer, a text editor): there is no viewer in
  this path.
- **Parameters, not rebuilt definitions.** The recipient can change the
  parameters of Code and Input Cards (`--set card.param=value`,
  `--params file.json`; `--help` lists them). Parameters marked `nix` change
  the Card's build, so they are fixed at export; the app refuses them rather than attempting a build offline.
- **Files through the input Card.** `--file CARD=PATH` replaces the file of
  an input Card whose output is a File. The core reads an input Card's file
  only from the Card's own folder, so the app copies the recipient's file
  into the copy of the Project there (`recipient/<name>`) and points the Card
  at it. An input Card is not built, so this changes no build.
- **Values are recorded in the app.** Record I/O is off by default, which
  would leave the recipient without recorded output values. The app's copy turns it on
  for every Code Card; it is a setting, outside what a build evaluates.
- **Size.** What remains is what the Cards and tsugumid need to run.
  Cell-count's payload (402 MiB) is its Cards' closures: Python 147 MiB,
  SciPy 135, BLAS and LAPACK 55 each, NumPy 51, matplotlib 44, scikit-image
  34 (unpacked). The runner image (155 MiB; 335 unpacked) is mostly glibc
  47, ICU 40 (linked by tsugumid), python3-minimal 32 (the run driver),
  tsugumid 28, util-linux 30 and Boost 18 (through the Nix libraries
  tsugumid links). Smaller apps now depend on the Cards' own environments
  and on a tsugumid build without the evaluator for the runner.

## Layout

| Path | What |
| --- | --- |
| [guest/default.nix](guest/default.nix) | The export and runner images (`packages.aarch64-linux.pack-guest`) and their `/init` |
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
builder). `TSUGUMI_PACK_GUEST=DIR` uses a guest built elsewhere, such as
CI's `pack-guest-<commit>` artifact (the aarch64-linux platform job builds
it whenever `pack/` or the native runtime changes).

## First end-to-end check

`demos/en/cell-count` (seven Python and Bash Cards), exported and
run on an M-series Mac (14 CPUs; the guest gets 8 GiB), 2026-11-18. This
measurement predates the current Card ABI. The demo still uses the previous ABI and does not build
now. TODO: repeat the check once the demos are ported
([Demos](../docs/Demos.md)).

| | |
| --- | --- |
| | Now | Evaluating in the app (first version) |
| --- | --- | --- |
| Export, store disk warm | 22 s | 58 s |
| App size | 586 MiB: payload 402 (104 store paths), runner image 155, kernel 14 | 728 MiB: payload 544 (4,804 paths), image 170, kernel 14 |
| Download (`ditto -c -k` zip) | 396 MiB | 466 MiB |
| Boot to tsugumid ready | 0.6 s | 1.9 s |
| First Code Card result (`mitosis`) / first figure (`generate`) | 0.9 s / 1.7 s | 2.2–2.6 s / 3.6 s |
| Whole Workflow, results written | 21.8 s | 23.8–24.8 s |

The first export with an empty store disk took 77 s (builds 28 s). The
parameter sweep is 18 s of every run.

**Offline.** The app's guest has no network device at all, and its image
has no Nix, so nothing in it can download or build; tsugumid's log records
no evaluation. Every Card still ran, which shows the payload is complete. (The Mac's own network was left on: this Mac is
shared, and the guest's isolation does not depend on it.)

**Same results as the Workflow in Tsugumi.** The reference is the same
Project run by this branch's `tsugumid` directly on an online aarch64 NixOS
host (OrbStack), with its own store and the normal project location. All six
figures (PNG) and all seven Card logs are byte-identical to the app's. With a
new input, `--set generate.seed=11 --set generate.cells=120 --set
real.min_area=20`, the counts change (score: 96 by threshold, 117 by
watershed) and are again byte-identical to the reference given the same
values. Setting a `nix` parameter (`mitosis.hash`) is refused with its reason.

**A recipient's file.** A three-Card Project (trigger, an input Card with a
text file, a Python Card that counts lines and words and writes the text
upper-cased as a File) exported to a 268 MiB app (payload 98 MiB; 407 MiB
when the app evaluated). With `--file text=mine.txt` the app returned
`counts.json` (3 lines, 6 words) and `upper/upper.txt` for the given file in
1.8 s, identical to the reference
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
  services are not exported. Cards that use `nixSources` are refused by the
  app's tsugumid: their source lookups are evaluations too, and the records
  do not carry them yet.
