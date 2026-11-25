# The bundled demos

What Cards each bundled demo connects, how, and what happens when it runs.
Choose "Open a Copy" from "Browse Demos" on the home screen and an editable copy is made in `~/Documents/Tsugumi/<demo>` (the bundled original is not changed).
The demos come as a Japanese set and an English set (`ja/`, `en/`), the same demos with their text in one language; the app shows the set of its language. Choose the language in settings and inspect values in each demo's own run records.

The projects and reference material live in [tsugumi-demos](https://github.com/gen740/tsugumi-demos). Tsugumi pins that repository in `flake.lock` and exposes the pinned tree as `.#demos`. To change a demo, commit the English and Japanese changes together on `tsugumi-demos`'s `main`, push normally, then run `nix flake update tsugumi-demos` in Tsugumi and commit the lock update. Demos use the public project format, Card ABI and tsugumi-kit SDKs.
Draft entries with a finished counterpart declare both language-qualified counterpart folders in `agentExclude` in their catalog metadata. When the host creates a project from the demo, it appends those resource folders' absolute paths in this installation to the project's ordinary `agentExclude` list. Moving the application later leaves these entries stale; nothing repairs or relocates them automatically.

Operations on screen are in [Usage](Usage.md); kinds of Card and the rules of execution are in [Features](features.md#2-cards-and-workflows).

## Demo status

The projects pin their Nix sources in `tsugumi.lock`. Their Cards send
interactive displays to declared views and return results through typed ports
or retained artifacts. Live view messages are not saved in run history.
The site player can replay recorded timelines. Cas A and GW150914 need
external observation downloads. Glowroot needs interactive input.

| Demo | Subject | What it shows |
| --- | --- | --- |
| [Data to report](#data-to-report-report) | Aggregating a sales table into a report | Reproduction by seed, failure and `blocked`, rerunning one Card with saved inputs, artifacts, child Workflows |
| [Collatz (C++ / Python / Julia / R)](#collatz-c--python--julia--r-collatz) | The Collatz sequence | Cross-checking independent implementations in four languages, branches, merges and loops, `maxRuns`, detecting integer overflow |
| [Python → Ruby → C++](#python--ruby--c-ruby-pipeline) | Passing a typed integer across languages | Ruby Hash outputs, parameters and live displays |
| [Live Stream](#live-stream-live-stream) | Plotting a random walk as it goes | Feedback loops, cancellation and the SDK line chart |
| [Glowroot](#glowroot-glowroot) | Growing an incremental game from a sprout | Card in Card screens, a shared Dawn window, sound and pure rules with saved inputs |
| [Cas A · Chandra RGB](#cas-a--chandra-rgb-casa) | X-ray observation of a supernova remnant | Observation data pinned with Nix, rerunning per Card, version history |
| [Maximum flow and minimum cut](#maximum-flow-and-minimum-cut-max-flow) | Maximum flow and minimum cut | Checking max flow = min cut three ways, and that only bottlenecks change the flow |
| [Signal detection variants](#signal-detection-variants-signal-sweep) | A two-parameter signal detection sweep | Labelled run lists and Collect |
| [Interactive point cloud](#interactive-point-cloud-point-cloud) | A torus knot drawn in WebGPU | An interactive SDK points view |
| [GW150914](#gw150914-gw150914) | Gravitational wave strain | Fetching public data, filtering and comparing detectors |
| [Cross-language calibration](#csharp-calibration) | Calibrating measurements with C# | Typed ports and a retained CSV |
| [Go pipeline](#go-pipeline-go-pipeline) | Scaling an integer with Go | Typed ports and owned inputs and outputs |
| [Rust pipeline](#rust-pipeline-rust-pipeline) | Scaling an integer with Rust | Typed ports, borrowed inputs and owned outputs |
| [GW150914 Draft](#gw150914-draft-gw150914-draft) | A plan for the same analysis | The Agent implements Draft Cards |

The arrows in the diagrams are a Card's wires (`output port → input port`). The kinds of Card are trigger (where a run starts), merge, collect, subgraph (Card in Card) and code Cards ([supported languages](CardGuide.md#inputs-and-outputs)).
Every Card has a summary in `README.md`, readable on the Canvas (by hovering, for trigger and merge) and in the Card's settings.

## The basics

### Python → Ruby → C++ (`ruby-pipeline`)

Python emits the `value` parameter (21). Ruby multiplies it by `factor` (2), displays
input / factor / result in a table, and sends the typed result to C++. Its display
reads `C++ received 42`. Change Ruby's factor and rerun it to reuse the saved Python
input.

### Data to report (`report`)

Makes a sales table from a seed, aggregates it by region into a Markdown report, and finally files it away in a child Workflow.
The shortest Workflow, mostly Python Cards.

**Try**
- Change `seed` in `generate` or `heading` in `render` and run. With the same `seed` the aggregate is the same and only the heading changes.
- Turn `fail_on_purpose` of `aggregate` On and run. `aggregate`'s log gives the reason, and `render` downstream is `blocked`. Turn it Off and rerun only `aggregate`: with the saved inputs it succeeds at once.
- Double-click `archive` to go into the child Workflow, and preview and export the artifact's directory. Artifacts remain even after the run cache is cleared.

### Collatz (C++ / Python / Julia / R) (`collatz`)

The same Collatz iteration implemented separately in C++, Python, Julia and R, checking that the four sequences agree.
The iteration itself is drawn with the Cards' wires, so the behaviour of branches, merges and loops can be followed on the Canvas.

**How the Julia and R sides differ.** Every code Card of the four loops sets `reuseWorker: true`, so one worker per Card serves all the iterations instead of a new interpreter for each. Only the C++ and Python `collect` use the state that this keeps. This demo keeps the Julia and R sequence explicit in the messages: the value going back to `loop` carries "the sequence so far" (`Walk`: `value` and `path`), and `collect` outputs `path` as the sequence when 1 arrives. Because the message carries the process, the record of any Card shows the sequence up to that point.

**R's range.** R's numbers are doubles, so integers are exact only below 2^53. Larger values are passed by the worker as strings, so R's `check` and `halve` fail rather than round silently, and `triple` fails before computing when `3n+1` would reach 2^53. Within this range the four sequences agree. Julia computes in 64-bit integers (`UInt64`), and its `triple`, like C++ and Python, fails with `integer overflow` when `3n+1` exceeds 64 bits.

**Try**
- Run it as it is. 27 reaches 1 in 111 steps (112 values), with a maximum of 9232 on the way. `compare` reports that they agree.
- Set the `value` in each implementation’s `initial/params.nix` to `"2"`: the odd side's `triple` is never chosen and is `skipped`.
- Make `maxRuns` of `check` inside smaller: it stops at the iteration limit (`execution_limit`). With a starting value for which `3n+1` exceeds 64 bits, `integer overflow` is detected.
- Set each implementation’s `value` to the decimal string for a large integer such as 2^60: R's `check` fails, saying it is "not below 2^53" (Julia, C++ and Python return 61 values).
- Open `collatz_jl` or `collatz_r` and look at the record of `collect`: the live view shows the sequence so far.

### Signal detection variants (`signal-sweep`)

Run **start** to compare three signal strengths (8, 12, 16) with two detection
thresholds (10, 14). **score** maps both lists in `params.nix` to six labelled
implementations. Open the batch history to see the signal and threshold in
each label.

**collect** turns the six results into one `Collected` value. **summary** reads the
vector and shows three detections out of six comparisons.

**Try**

- Add a value to **score**'s `values`, for example `[ 8 12 16 20 ]`, and run again.
- Add a threshold such as `12` to `thresholds` and compare the two runs. Members
  with the same combination of value and threshold keep the same label.

[Run lists and Collect](features.md#run-lists-and-collect)

### Live Stream (`live-stream`)

Makes one value of a random walk every 100 ms. The `settings` Card opens the **seed** Dawn view, receives the chosen starting value with `recv` and emits the initial state into a Merge. The generator returns the next state to the Merge until it reaches the sample limit. The `plot` Card opens a line chart panel on the first sample and updates it as values arrive.

**Try**
- Start a run and click a tick on the **seed** slider when it turns blue. Stop (Cancel) in the middle. It stops at once, and the chart up to that point stays.
- Change `interval_ms`, `seed` or `samples` in `settings`, or `window` in `plot`, and run again. The new run starts a new chart.

### Glowroot (`glowroot`)

Click the sprout to gather light and buy roots for passive income. Replant for
seeds, spend them on skills, then buy Bloom after the third replant.

`title/page` opens a Dawn window and sends its session with the game state.
Each screen is a subgraph. The grow, skills, replant and ending pages take turns
drawing to the same window.
After playing, open `replant/apply` or `skills/validate` and choose **Run Only This
Card (Last Inputs)** to try a rule with the saved state. Adjust the game's
balance in the Cards' `params.nix` files.

**Try**
- Open the grow, skills and replant Workflows to follow how the state changes
  between screens. Each screen passes the state through its boundary ports.
- Inspect the saved inputs of a purchase or replant rule and rerun it with a
  different price or reward.

### Cas A · Chandra RGB (`casa`)

Pins with Nix the X-ray event data of the supernova remnant Cassiopeia A (Chandra) and the analysis tool CIAO, and splits the photons into three energy bands for an RGB image.
It also draws the energy spectrum of the same photons, to check which element's lines became which colour.

**Try**
- Run it as it is. The default observation is ObsID 25628 (2.1 ks, 13 MB). Read the photon-count table in `bands` and compare it with the RGB image.
- Change `obsid`, `file` and `hash` of `load` to ObsID 114 (2000, 50 ks, 520 MB) or ObsID 19606 (2019, 49 ks, 349 MB): the deeper observations give a sharper image, and compare the observations (the download settings are on the sticky note and in `load`'s description). Only the first run takes time to fetch.
- Change the bands of `bands`, the stretch of `rgb`, or the range and binning of `spectrum`, and rerun just that Card. The spectrum shows that the green band contains exactly the Si line, and everything from S up falls in blue.
- Open the version tree. It holds the process of trying bands, grids and stretches: failed branches, branches not adopted, the adopted version `rgb-v1`, the version that added the spectrum on top, and the version that pinned CIAO ([Versions](Versions.md#demo-histories)).

### Interactive point cloud (`point-cloud`)

`cloud` generates 400,000 points along a torus knot and sends them to the SDK's
WebGPU points view. Drag to orbit and scroll to zoom. Change the point count
in the Card's settings and run again to inspect how sampling density changes.

### GW150914 (`gw150914`)

Downloads 32 seconds of public H1 and L1 strain data from GWOSC. `whiten`
filters the two detectors' data and `chirp` constructs a spectrogram.
Change `f_low` or `f_high` to compare the extracted waveform and detector
overlap. Rerun the analysis using its saved inputs to reuse the download.

## Demos that compute locally

### Maximum flow and minimum cut (`max-flow`)

On a made-up layered network made from a seed, checks that the maximum flow equals the minimum cut. You can compare which edges increase the maximum flow when widened, and which do not.

**Try**
- Run it as it is and look at `sweep`. Of the cut's three edges only two increase it; `a2->b2` does not, because another minimum cut remains (`verify` counts two minimum cuts).
- Set `edge` of `whatif` to `s->a2` and `raise_by` to 1000. Even an edge running at full capacity does not increase the maximum flow if it is off the cut.
- Set `edge` of `whatif` to `b3->c3`. Raising it by 10 increases the flow by only 8, because the next cut gets in the way; increasing `raise_by` further gains nothing beyond that.
- Change `seed` of `network` and run the whole. On another network, the values in `verify` still all agree.

### Go pipeline (`go-pipeline`)

Python emits a typed integer. Go unpacks it into an owned value, applies
`factor` and returns a typed result to C++. Change `factor` in the Go
Card and run it with the saved input to check the new product.

### Rust pipeline (`rust-pipeline`)

Python emits the same typed integer through a FlatBuffers port. Rust reads
the borrowed input, multiplies it by `factor` and sends an owned result to
C++. Change `factor` and rerun the Rust Card with its last input.

## GW150914 Draft (`gw150914-draft`)

A start Card connects to three Drafts for downloading, whitening and
plotting the public strain data. Each Draft describes the inputs,
calculation and result. Choose **Generate Cards** to have the Agent turn
the plan into a runnable Workflow. [Generating Cards from Drafts](Usage.md#working-with-the-agent)

## csharp-calibration

The **Cross-language calibration (C#)** demo passes three C++ readings (1.25, 2.5, 3.75) through a C# Card with `factor = 1.6`, then summarizes the typed result in Python and retains a CSV. The expected calibrated values are 2, 4 and 6: sum 12 and mean 4. Change `calibrate.factor` to 2 and rerun; the sum becomes 15 and mean 5. The C# log uses a fixed-hash Newtonsoft.Json dependency. Each Card's source and the saved CSV remain available to inspect. See the [C# SDK guide](https://github.com/gen740/tsugumi-kit/blob/main/sdk/csharp/README.md) for the SDK and worker lifetime.
