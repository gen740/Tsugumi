# The bundled demos

What Cards each bundled demo connects, how, and what happens when it runs.
Choose "Open a Copy" from "Browse Demos" on the home screen and an editable copy is made in `~/Documents/Tsugumi/<demo>` (the bundled original is not changed).
The demos come as a Japanese set and an English set (`demos/ja`, `demos/en`), the same demos with their text in one language; the app shows the set of its language. The values below are from the English set, and the Japanese set gives the same numbers.
Operations on screen are in [Usage](Usage.md); kinds of Card and the rules of execution are in [Features](features.md#2-cards-and-workflows).

| Demo | Subject | What it shows |
| --- | --- | --- |
| [Data to report](#data-to-report-report) | Aggregating a sales table into a report | Reproduction by seed, failure and `blocked`, rerunning one Card with saved inputs, artifacts, child Workflows |
| [Collatz (C++ / Python / Julia / R)](#collatz-c--python--julia--r-collatz) | The Collatz sequence | Cross-checking independent implementations in four languages, branches, merges and loops, `maxRuns`, detecting integer overflow |
| [Python → Ruby → C++](#python--ruby--c-ruby-pipeline) | Passing a typed integer across languages | Ruby Hash outputs, parameters, resident run-start state, live displays |
| [Live Stream](#live-stream-live-stream) | Plotting a random walk as it goes | A stream made by a feedback loop, stopping at once, WebGPU line charts |
| [Plots](#plots-plots) | Diffusion of random walks | Ordinary matplotlib (`plt.show()`, `FuncAnimation`) drawn straight into a Card; Plotly and Altair become interactive charts |
| [Cas A · Chandra RGB](#cas-a--chandra-rgb-casa) | X-ray observation of a supernova remnant | Observation data pinned with Nix, rerunning per Card, version history |
| [Science demos](#science-demos-fetching-and-analysing-public-data) | Gravitational waves, a protein, an exoplanet, earthquakes | Fetching public data (sizes and SHA-256), analysing and plotting it |
| [MNIST · comparing classifiers](#mnist--comparing-classifiers-mnist-classifiers) | Classifying handwritten digits (seven methods, from kNN to a CNN) | Seven branches from one input running at once in separate Nix environments and merging; rerunning just one branch with other conditions |
| [Newton's method](#newtons-method-newton) | Newton's method on the complex plane | The starting point decides where it goes and how fast; multiple roots and periodic orbits lose quadratic convergence |
| [Pharmacokinetics](#pharmacokinetics-pharmacokinetics) | Blood concentration of a made-up drug | The dosing interval changes accumulation, peaks and troughs; the numerical solution is checked against closed forms (teaching material, not dosing guidance) |
| [Maximum flow and minimum cut](#maximum-flow-and-minimum-cut-max-flow) | Maximum flow and minimum cut | Checking max flow = min cut three ways, and that only bottlenecks change the flow |
| [Dose–response curve](#doseresponse-curve-dose-response) | A dose–response curve (synthetic data) | Fitting a four-parameter logistic, with bootstrap intervals; the interval opens up when the concentration range does not bracket the EC50 |
| [Molecular properties](#molecular-properties-molecules) | Properties of published molecules (RDKit) | Properties, structures and similarity from SMILES; paste a molecule to try it, and an invalid SMILES says why |
| [Counting cells in images](#counting-cells-in-images-cell-count) | Counting cells (synthetic data and a real image) | Scoring thresholding and watershed on images with known answers, and seeing where they break down |
| [Survival analysis](#survival-analysis-survival) | Survival in lung cancer (`lung` from the survival package) | How the hazard ratio moves with the subjects and the adjustment, side by side; checking the proportional hazards assumption |
| [Meta-analysis: the BCG vaccine](#meta-analysis-the-bcg-vaccine-meta-analysis) | A meta-analysis of the BCG vaccine (`dat.bcg` from metafor) | Fixed and random effects, heterogeneity, leaving one trial out, meta-regression on latitude |
| [RNA-seq: airway and dexamethasone](#rna-seq-airway-and-dexamethasone-rna-seq) | RNA-seq (airway and dexamethasone) | DESeq2 and edgeR compared on separate branches; whether to include the cell line changes the number of genes a lot |
| [A correlation that reverses when stratified](#a-correlation-that-reverses-when-stratified-simpson) | Simpson's paradox (penguins, and admissions at Berkeley) | Correlations that reverse direction between the aggregate and the strata, and that which to read depends on the question |
| [Scheduling a battery](#scheduling-a-battery-battery) | Charging and discharging a battery (synthetic prices) | How the optimal plan from dynamic programming changes with capacity and prices; at a flat price, not using it is optimal |
| [Fluctuations in chemical reactions](#fluctuations-in-chemical-reactions-gillespie) | Gillespie's stochastic simulation | How it differs from rate equations; stationary distributions checked against theory; fluctuations growing as molecule numbers shrink |
| [An inverse problem in heat conduction](#an-inverse-problem-in-heat-conduction-heat-inverse) | Recovering an initial temperature from measurements | The naive inverse breaks down; how the strength of Tikhonov regularisation and the measurement time change the recovery |
| [Comparing ecological models](#comparing-ecological-models-ecology-models) | Fur records of hares and lynx (1900–1920) | Fitting predator–prey models and comparing them by AICc and by forecasting unseen years; a good score does not mean the mechanism is right |
| [Draft versions](#draft-versions-for-the-agent-to-implement) | Only the plans of the science demos above and of Cas A | The Agent generates Cards from Drafts, runs them and checks |
| [Dynamical systems gallery](#dynamical-systems-gallery-dynamics-draft) | Six dynamical systems | The Agent splits independent Drafts among helpers and implements them at the same time |
| [MNIST · comparing classifiers (Draft)](#mnist--comparing-classifiers-draft-mnist-classifiers-draft) | Classifying handwritten digits (seven methods) | From a plan implemented up to the data, helpers implement seven classifiers at the same time and the Agent puts the comparison together |

The arrows in the diagrams are a Card's wires (`output port → input port`). The kinds of Card are trigger (where a run starts), input (an input whose value is edited on the Canvas), fork (distribution), branch (conditional branching), merge, subgraph (Card in Card) and code Cards (C++ / Python / bash / R / Julia / Ruby).
A fork Card only hands values out as the arrows show, so it is left out of each demo's table.
Every Card has a summary in `_meta.json`, readable on the Canvas (by hovering, for trigger, fork, merge and branch) and in the Card's settings.

## The basics

### Python → Ruby → C++ (`ruby-pipeline`)

Python emits the `value` parameter (21). Ruby multiplies it by `factor` (2), displays
input / factor / result in a table, and sends the typed result to C++. Its display
reads `C++ received 42`. Change Ruby's factor and rerun it to reuse the saved Python
input. Ruby's `on_run_start` counts Runs in its resident Card instance and writes that
count to the log; turn off worker reuse to start with fresh state.


### Data to report (`report`)

Makes a sales table from a seed, aggregates it by region into a Markdown report, and finally files it away in a child Workflow.
The shortest Workflow, mostly Python Cards.

```text
start → settings ─ share ─┬→ generate ─csv→ aggregate ─summary→ render ─report→ archive (child Workflow)
                          └──────────────── config ──────────────→ render
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends `seed` (42), `rows` (500) and `heading` |
| `generate` | Python | Writes a CSV of `rows` rows of region, product, quantity and unit price with random numbers from `seed`. The same `seed` gives the same CSV |
| `aggregate` | Python | Totals sales and quantity per region. With `fail_on_purpose` On, it fails on purpose (`recordIO` records its input and output values) |
| `render` | Python | Writes `report.md` and `summary.json` into a directory and displays the report (`retainArtifact` keeps the artifact) |
| `archive` | subgraph | The bash Card inside, `copy`, duplicates the report's directory and adds `ARCHIVED_AT` |

**Try**
- Change `seed` or `heading` and run. With the same `seed` the aggregate is the same and only the heading changes.
- Turn `fail_on_purpose` of `aggregate` On and run. `aggregate`'s log gives the reason, and `render` downstream is `blocked`. Turn it Off and rerun only `aggregate`: with the saved inputs it succeeds at once.
- Double-click `archive` to go into the child Workflow, and preview and export the artifact's directory. Artifacts remain even after the run cache is cleared.

### Collatz (C++ / Python / Julia / R) (`collatz`)

The same Collatz iteration implemented separately in C++, Python, Julia and R, checking that the four sequences agree.
The iteration itself is drawn with the Cards' wires, so the behaviour of branches, merges and loops can be followed on the Canvas.

```text
                        ┌→ collatz-cpp (child Workflow) ─┐
start → initial ─ both ─┼→ collatz-py  (child Workflow) ─┼→ compare ─sequence→ record
                        ├→ collatz-jl  (child Workflow) ─┤
                        └→ collatz-r   (child Workflow) ─┘
```

The outer Cards:

| Card | Kind | What it does |
| --- | --- | --- |
| `initial` | input | Sends the starting value (27) |
| `collatz-cpp` / `collatz-py` / `collatz-jl` / `collatz-r` | subgraph | Iterates in the Workflow inside and returns the sequence until it reaches 1 |
| `compare` | Python | Checks that the four sequences agree exactly and passes the sequence on |
| `record` | Python | Writes the sequence to `collatz.csv` (`k, a_k, parity`) and keeps it as an artifact |

Inside the child Workflow (the same shape in all four):

```text
(boundary input n) → loop ─ split ─┬→ check ─value / even→ route ─a (even)→ halve ─┐
                      ↑             └→ collect → (boundary output sequence) └b (odd)→ triple ─┤
                      └──────────────────────────── half / triple ──────────────────────────┘
```

| Card | Kind | What it does |
| --- | --- | --- |
| `loop` | merge | Merges the first value with the values coming back from `halve` and `triple` |
| `check` | code | Outputs nothing if the value is 1 (ending the loop); otherwise outputs the value and its parity. `maxRuns` (200) bounds the iterations |
| `route` | branch | Fires only `a` (`halve`) for even, or only `b` (`triple`) for odd |
| `halve` / `triple` | code | Compute `n/2` and `3n+1`. `triple` fails with `integer overflow` beyond 64-bit integers |
| `collect` | code | Collects and displays the values that arrive, and outputs the sequence when 1 arrives. C++ and Python keep the values in the worker (`reuseWorker: true`) and clear them for each run in `onRunStart` |

**How the Julia and R sides differ.** Every code Card of the four loops sets `reuseWorker: true`, so one worker per Card serves all the iterations instead of a new interpreter for each. Only the C++ and Python `collect` use the state that this keeps. This demo keeps the Julia and R sequence explicit in the messages: the value going back to `loop` carries "the sequence so far" (`Walk`: `value` and `path`), and `collect` outputs `path` as the sequence when 1 arrives. Because the message carries the process, the record of any Card shows the sequence up to that point.

**R's range.** R's numbers are doubles, so integers are exact only below 2^53. Larger values are passed by the worker as strings, so R's `check` and `halve` fail rather than round silently, and `triple` fails before computing when `3n+1` would reach 2^53. Within this range the four sequences agree. Julia computes in 64-bit integers (`UInt64`), and its `triple`, like C++ and Python, fails with `integer overflow` when `3n+1` exceeds 64 bits.

**Try**
- Run it as it is. 27 reaches 1 in 111 steps (112 values), with a maximum of 9232 on the way. `compare` reports that they agree.
- Set `initial` to 2: the odd side's `triple` is never chosen and is `skipped`.
- Make `maxRuns` of `check` inside smaller: it stops at the iteration limit (`execution_limit`). With a starting value for which `3n+1` exceeds 64 bits, `integer overflow` is detected.
- Set `initial` to a large value such as 2^60: R's `check` fails, saying it is "not below 2^53" (Julia, C++ and Python return 61 values).
- Open `collatz-jl` or `collatz-r` and look at the record of `collect`: each invocation's display is the sequence so far.

### Signal detection sweep (`signal-sweep`)

Run **start** to compare three signal strengths (8, 12, 16) with two detection
thresholds (10, 14). **samples** emits the three signals. **score** reads the
thresholds from `inputs.signal.thresholds` in its Nix definition and makes six
comparisons. Open its batch history to see which signal and threshold each
invocation used.

**collect** turns the six results into one typed vector. **summary** reads the
vector and shows three detections out of six comparisons. Edit the thresholds
in `samples/default.nix` and run from **start** again to see which detections
change. [Run lists and Collect](features.md#run-lists-and-collect)

### Live Stream (`live-stream`)

Makes one value of a random walk every 100 ms and draws the latest values as a line chart in real time.
A feedback loop that sends "the next state" back to itself makes a stream that ends.

```text
start → settings ─state→ loop → generate ─sample→ plot
                          ↑         │
                          └─ next ──┘
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends `seed` (7), `samples` (200), `interval_ms` (100) and the initial state |
| `loop` | merge | Merges the initial state with the next state `generate` returns |
| `generate` | C++ | Waits `interval_ms`, then takes one step with a random number decided by `seed` and the index, and outputs the value and the next state. After `samples` it stops outputting the next state and the stream ends. It checks for stopping while waiting too |
| `plot` | Python | Draws the latest `window` (60) values and their mean with a WebGPU line chart |

**Try**
- Run it and stop (Cancel) in the middle. It stops at once, and the chart up to that point stays.
- Change `interval_ms`, `seed`, `samples` or `window` of `plot` and run again. The series resets, and with parallelism 1 or 2 no value goes missing or out of order.

### Plots (`plots`)

Makes random walks and plots them with the same matplotlib, Plotly and Altair code you would write anywhere else.
No Tsugumi-specific drawing calls are used. Python Cards use the SDK's matplotlib backend, so a figure drawn with `plt.show()` appears as the Card's display. Plotly's `fig.show()` and Altair's `chart.show()` appear as interactive charts.

```text
start → settings ─walk→ simulate ─walks→ share ─┬→ summary
                                                ├→ diffusion
                                                ├→ explore
                                                └→ spread
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends `seed` (7), `walkers` (400) and `steps` (200) |
| `simulate` | Python | Makes `walkers` ±1 random walks of `steps` steps with numpy |
| `summary` | Python | With `plt.show()`, draws 40 trajectories with the ±2√t band, and a histogram of final positions with the normal distribution, in one figure |
| `diffusion` | Python | With `FuncAnimation`, plays live how the distribution at each time spreads as √t (`frames` frames) |
| `explore` | Python | With Plotly's `fig.show()`, a scatter plot of each walker's final position and farthest distance (coloured by the fraction of time above 0) |
| `spread` | Python | With Altair's `chart.show()`, draws the mean squared distance growing in proportion to t, next to the theoretical value |

**Try**
- Run it: `diffusion`'s display plays as an animation, and the last frame stays.
- Enlarge the displays of `explore` and `spread`, and try zooming, hovering (values per walker) and the legend.
- Change `walkers`, `steps`, `seed` or `frames` and run again.

### Cas A · Chandra RGB (`casa`)

Pins with Nix the X-ray event data of the supernova remnant Cassiopeia A (Chandra) and the analysis tool CIAO, and splits the photons into three energy bands for an RGB image.
It also draws the energy spectrum of the same photons, to check which element's lines became which colour.

```text
start → begin ─┬→ ciao ─tool──┐
               └→ load ─events┴→ bands ─images→ share ─┬→ rgb
                                                       └→ spectrum
```

| Card | Kind | What it does |
| --- | --- | --- |
| `ciao` | bash | In `default.nix`, builds the CIAO 4.18 source pinned by hash and passes on the store path and version. The build happens only the first time and takes a few minutes. The expression is read from [gen740/ciao](https://github.com/gen740/ciao), pinned by commit and hash (`nixSources` of `tsugumi.json`; [external Nix sources](features.md#external-nix-sources-inputssources)) |
| `load` | bash | In `default.nix`, fetches and unpacks HEASARC's observation file pinned by hash (explicit FTPS). The download happens only the first time; after that the file in the Nix store is used. It uses no network while running |
| `bands` | Python | Splits the photons into three bands, 600–1650 / 1650–2250 / 2250–7500 eV, and counts them with CIAO's `dmcopy` on a 360×360 grid centred on Cas A (RA 350.8584°, Dec +58.8113°). The total counted is checked against the number read with astropy, without CIAO. Writes a FITS file per band and `provenance.json`, and shows the photon counts as a table |
| `rgb` | Python | Assigns the three bands to R, G and B and makes a PNG stretched with asinh (`percentile` 99.5, `q` 10). Records the display conditions and the PNG's SHA-256 |
| `spectrum` | Python | Draws a histogram of the energies of the photons in the same grid with matplotlib. Shades the three bands and marks the lines of Mg, Si, S, Ar, Ca and Fe |

**Try**
- Run it as it is. The default observation is ObsID 25628 (2.1 ks, 13 MB). `bands` shows the photon counts per band (red 101,187 / green 157,471 / blue 110,852) as a table.
- Change `obsid`, `file` and `hash` of `load` to ObsID 114 (2000, 50 ks, 520 MB) or ObsID 19606 (2019, 49 ks, 349 MB): the deeper observations give a sharper image, and about twenty years of change can be compared (the values are on the sticky note and in `load`'s description). Only the first run takes time to fetch.
- Change the bands of `bands`, the stretch of `rgb`, or the range and binning of `spectrum`, and rerun just that Card. The spectrum shows that the green band contains exactly the Si line, and everything from S up falls in blue.
- Open the version tree. It holds the process of trying bands, grids and stretches: failed branches, branches not adopted, the adopted version `rgb-v1`, the version that added the spectrum on top, and the version that pinned CIAO ([Versions](Versions.md#demo-histories)).

## Science demos (fetching and analysing public data)

Four demos with the same shape: `load` fetches data from its publisher, and the two Cards that follow analyse it into figures and tables.

```text
start → load ─data→ (analysis 1) → (analysis 2)
```

`load` is a Python Card allowed the network (`network: true`). It fetches the files of `urls` and `names`, passes them on as a folder, and shows the size and SHA-256 of each.
Where Cas A's `load` pins the fetch with Nix, these fetch on every run.

| Demo | Data fetched | Analysis 1 | Analysis 2 |
| --- | --- | --- | --- |
| **GW150914 · gravitational waves** (`gw150914`) | GWOSC's H1 and L1 strain data (4 kHz, 32 s of HDF5) | `whiten`: whitens with the PSD of a 4 s Welch estimate, applies a band-pass from `f_low` to `f_high` (35–350 Hz), and plots the waveform around the event | `chirp`: shifts L1 by ±10 ms to find the delay that best overlaps H1, overlays them, and shows the chirp in H1's spectrogram. Also estimates a rough SNR |
| **6LU7 · a protein and its binding pocket** (`protein-6lu7`) | 6LU7 from RCSB PDB (the SARS-CoV-2 main protease with the inhibitor N3) | `structure`: takes out the coordinates of the Cα atoms and the inhibitor (chain C) and shows them as a 3D point cloud (Cα blue, inhibitor red) | `pocket`: tables the residues within `cutoff` (4 Å) of the inhibitor and marks the pocket's residues on a contact map. You can check that the catalytic His41 and Cys145 are among them |
| **Kepler-6 b · an exoplanet transit** (`exoplanet-transit`) | MAST's quarter-3 light curve of Kepler-6 (KIC 10874614) (FITS) | `detrend`: normalises the points with quality flag 0, removes the trend with a moving median of `window_days` days, and searches for the period with Box Least Squares | `fold`: folds the phase by the period and finds the planet-to-star radius ratio Rp/R* = √δ from the depth δ of the dip (a period of about 3.23 days, a depth of about 1%) |
| **Japan Trench · hypocentre sections and the b-value** (`seismic-trench`) | USGS hypocentres of M4 and above, 2000–2024, 34–42°N and 138–146°E (CSV) | `section`: cuts out the band of latitude from `lat_min` to `lat_max` and draws a map of epicentres and a longitude–depth section in which the subducting Pacific plate shows | `b_value`: plots the cumulative frequency log10 N(≥M) and finds the b-value of the Gutenberg–Richter law by maximum likelihood (Aki–Utsu) above `mc` |

**Try**: run it as it is, then change a parameter of an analysis Card (the pass band, `cutoff`, `window_days`, the band of latitude, `mc` and so on) and rerun just that Card. The analysis alone is redone with the saved inputs, without fetching again.

## Demos that compute locally

Demos that use no network while running. Their data is synthesised, comes bundled with a package, or is pinned with Nix (fetched once, at build time). Their runs are checked by tests with `TSUGUMI_TEST_DEMO_RUNS=1`.

### MNIST · comparing classifiers (`mnist-classifiers`)

```text
start → load → prepare → share ─┬→ knn ──────┐
                                ├→ logistic ─┤
                                ├→ svm ──────┤
                                ├→ forest ───┤
                                ├→ boosting ─┼→ compare → views ─┬→ tradeoff
                                ├→ mlp ──────┤                   ├→ recall
                                ├→ cnn ──────┤                   └→ hard
                                └────────────┘ (test images and labels)
```

| Card | Kind | What it does |
| --- | --- | --- |
| `load` | Python (numpy, matplotlib) | Pins the four MNIST IDX files with `fetchurl` and hashes in `default.nix`. Checks the headers and counts, and shows samples of each digit with the source. Uses no network while running |
| `prepare` | Python (numpy) | Draws a stratified sample by `seed`, `train_size` (default 10000) and `test_size` (default 2000), scales pixels to 0–1 and passes them as an npz (a File port, kept as an artifact). Shows the count per digit |
| `knn` | scikit-learn | k-nearest neighbours (`k`). Shows the digits it got wrong and their neighbours |
| `logistic` | scikit-learn | Logistic regression (`c`, `max_iter`). Shows each digit's weights as an image |
| `svm` | scikit-learn | An SVM with an RBF kernel (`c`, `gamma`). Shows the number of support vectors per digit |
| `forest` | scikit-learn | A random forest (`trees`, `max_depth`, `seed`). Shows an image of the pixels that mattered |
| `boosting` | xgboost | Gradient boosting (`rounds`, `max_depth`, `eta`, `seed`). Shows the test error rate per round |
| `mlp` | torch (CPU) | A fully connected network with two hidden layers (`hidden`, `epochs`, `seed`). Shows how the loss went |
| `cnn` | torch (CPU) | A small network with two convolutional layers (`channels`, `epochs`, `seed`). Shows what the first layer picks out of test images |
| `compare` | Python (numpy) | Waits for all branches. Checks that they used the same test images (digest), recomputes the accuracy from the predictions to cross-check, and shows a ranking and, in words, whether the gap between first and second means anything |
| `tradeoff` | Plotly | A scatter plot of accuracy against training and prediction time (log axes, Pareto-optimal points joined by a line, details on hover) |
| `recall` | matplotlib | A table of recall by model × digit |
| `hard` | matplotlib | The 32 test images that the most models got wrong, and the most common wrong answers |

Each classifier Card shows its accuracy, single-thread CPU time (training and prediction) and a breakdown of its mistakes, and passes its predictions for all test images to `compare` as a typed message (`Result`).

**Try**

- Run it as it is. The seven branches run at the same time (how many at once is "Parallel runs" in the settings, default 4). The accuracy on 2000 test images is `svm` 0.9705, `cnn` 0.9680, `mlp` 0.9625, `knn` 0.9500, `forest` 0.9475, `boosting` 0.9455 and `logistic` 0.9210, and `compare` says the gap between `svm` and `cnn` does not reach twice the noise.
- Set `max_depth` of `forest` to 3 and run just that Card. No other branch runs, and only `forest`'s accuracy drops, from 0.9475 to 0.7570.
- To compare again, run from `forest` onwards ("Run from Here Downstream"). Only `forest` and what follows `compare` run, and `compare` compares the new `forest` result with the saved inputs of the other branches and `share`. The branches you did not change do not run, so their accuracy is the same.
- Change `k` of `knn`, `gamma` of `svm` or `epochs` of `cnn`.
- Increase `train_size` of `prepare` and see which classifiers benefit from more images and whose time grows (`svm`'s and `knn`'s prediction times grow).

The data is MNIST by LeCun, Cortes and Burges, fetched from PyTorch's mirror `https://ossci-datasets.s3.amazonaws.com/mnist/`.

### Newton's method (`newton`)

Solves $z^3 - 1 = 0$ on the complex plane to see how the starting point decides where it goes and how fast it converges. Alongside estimates of quadratic convergence and a map of where each point converges, it shows cases that lose quadratic convergence and cases that do not converge.

```text
start → settings → roots ─ share ─┬→ orbits ─errors→ order
                                  ├→ basins
                                  └→ trouble
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends the polynomial's real coefficients `coeffs` (highest degree first; default `[1, 0, 0, -1]`), the damping factor `damping`, `tolerance` and `max_iter` |
| `roots` | Python | Finds the roots, their multiplicities and the critical points without Newton's method, from the eigenvalues of the companion matrix (`numpy.roots`). Checks them by the relations between roots and coefficients, and for the form $z^n = c$ against the closed form too |
| `orbits` | Python | Plots the error against the iteration on a log axis, for each starting point `start_re`, `start_im` |
| `order` | Python | Estimates the order of convergence $q$ and its constant from the sequence of errors, and tables them next to the theoretical values set by the multiplicity and $\lambda$ |
| `basins` | Python | Starts from every point of a grid and draws which root it reaches and in how many iterations. Zoom in with `center_re`, `center_im` and `half_width` |
| `trouble` | Python | Shows side by side the reference case, near a critical point (`offset`), a multiple root, a multiple root brought back to quadratic with $\lambda = 2$, and a cycle of period 2 (the polynomials are fixed) |

**Try**
- Run it as it is. The errors in `orbits` fall quadratically on the log axis, and the estimated order in `order` lines up with the theory. `basins` draws the fractal boundaries between where points converge.
- Set `center_re` of `basins` to -0.8 and `half_width` to 0.2 and rerun just that Card. Wherever you zoom on the boundary, all three colours gather.
- Set `offset` of `trouble` to 1e-5. The first step is thrown out to $1/(3 z_0^2)$, and far away it comes back only by 2/3 per step, so the iterations go from 37 to 60 (exactly the default `max_iter`; smaller still, `max_iter` must be increased).
- Set `damping` of `settings` to 0.5 and run the whole. From every starting point the order becomes 1 with ratio 0.5, and the shape of the boundaries on the map changes too.
- Set `coeffs` of `settings` to `[1, 0, -3, 2]` ($(z-1)^2(z+2)$). At the root of multiplicity 2 the order drops to 1 with ratio 0.5, and the error stops at about $10^{-8}$.
- Set `coeffs` of `settings` to `[1, 0, -2, 2]` ($z^3 - 2z + 2$). A black region appears around 0 (starting points drawn into a cycle of period 2 that never converge).

### Pharmacokinetics (`pharmacokinetics`)

Compares how concentration moves with the dosing interval, in a one-compartment oral model of a made-up drug, "Tsugumicin". **This is a made-up model for teaching. Its values are not those of any real drug, and it is neither medical advice nor dosing guidance.** The therapeutic range is invented too.

```text
start → settings → simulate ─curves→ share ─┬→ check
                                            ├→ window
                                            └→ missed
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends the dosing plans to compare (`doses_mg`, `intervals_h`), `half_life_h`, `ka_per_h`, `volume_l`, `bioavailability`, the invented therapeutic range (`low_mg_l`, `high_mg_l`) and `days` |
| `simulate` | Python | Solves the differential equation with `solve_ivp`, split at each dose, and plots the concentration over time for the plans side by side with Plotly |
| `check` | Python | Tables the superposition of closed forms, the steady-state accumulation ratio $R$ and the mean concentration against the numerical solution. Fails if they differ much |
| `window` | Python | Overlays the concentration in the last dosing interval and compares peaks, troughs and the fraction of time inside the therapeutic range |
| `missed` | Python | Plots, with closed forms, the concentration per plan when `missed_count` doses from `missed_at_h` on are missed |

**Try**
- Run it as it is and look at `window`. 400 mg every 24 hours and 200 mg every 12 hours give the same mean concentration (the table in `check`), and shortening the interval narrows the gap between peak and trough.
- Set `missed_count` of `missed` to 2 and rerun just that Card. For the same daily dose, the longer the interval, the more the time below the range grows.
- Set `half_life_h` of `settings` to 16 and run the whole. The accumulation ratio grows, and so does the time to reach steady state.
- Shorten `days` of `settings` and "reaching steady state" in `check` shows that it does not get there.

### Maximum flow and minimum cut (`max-flow`)

On a made-up layered network made from a seed, checks that the maximum flow equals the minimum cut. You can compare which edges increase the maximum flow when widened, and which do not.

```text
start → settings → network → flow → cut ─ share ─┬→ verify
                                                 ├→ whatif
                                                 └→ sweep
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends `seed` (53), `layers`, `width`, `edge_probability` and `max_capacity` |
| `network` | Python | Makes a layered directed graph with capacities from the seed (made-up data) |
| `flow` | Python | Finds the maximum flow with networkx and draws each edge's flow/capacity. Checks capacities and conservation |
| `cut` | Python | Finds the minimum cut with networkx and draws the $s$ side, the $t$ side and the cut's edges. Fails unless max flow = min cut |
| `verify` | Python | Checks that scipy's `maximum_flow` and brute force over all cuts give the same value, and counts the minimum cuts |
| `whatif` | Python | Raises the capacity of an edge on the cut and one off it by `raise_by`, and tables the maximum flow and the new cut. `edge` (for example `a1->b1`) chooses the edge |
| `sweep` | Python | For every edge, bars the increase in maximum flow when its capacity is raised by `raise_by` |

**Try**
- Run it as it is and look at `sweep`. Of the cut's three edges only two increase it; `a2->b2` does not, because another minimum cut remains (`verify` counts two minimum cuts).
- Set `edge` of `whatif` to `s->a2` and `raise_by` to 1000. Even an edge running at full capacity does not increase the maximum flow if it is off the cut.
- Set `edge` of `whatif` to `b3->c3`. Raising it by 10 increases the flow by only 8, because the next cut gets in the way; increasing `raise_by` further gains nothing beyond that.
- Change `seed` of `settings` and run the whole. On another network, the values in `verify` still all agree.

### Dose–response curve (`dose-response`)

Makes plates of made-up compounds A and B with random numbers (synthetic data), fits a four-parameter logistic (Hill curve), and puts confidence intervals on the EC50 and the Hill slope.
The true values are known, so you can check on the spot whether they were recovered. These are not real drugs, and this is not medical advice.

```text
start → generate ─plate→ share ┬→ fit ─fit─────────→ bootstrap ─intervals→ compare
                               ├──────── plate ────→ bootstrap
                               └→ range
```

| Card | Kind | What it does |
| --- | --- | --- |
| `generate` | Python | Makes responses with normal noise on the true curve, from random numbers by `seed`. `concentrations` concentrations, diluted by `dilution` from `top_um`, with `replicates` replicates each |
| `fit` | Python | Finds bottom, top, log10 EC50 and the Hill slope with scipy's `curve_fit`, and plots the data and the curve |
| `bootstrap` | Python | Finds 95% intervals for the EC50 and the Hill slope by residual bootstrap (`n_boot` times), and tables whether they contain the true values. If an interval leaves the measured range, it warns, with the reason |
| `compare` | Python | Shows the ratio of EC50s (relative potency) with its interval, and the difference of Hill slopes. If the interval contains 1, it says "no difference can be claimed" |
| `range` | Python | Remakes synthetic plates changing only the highest concentration, and plots the width of the EC50 interval against it |

**Try**
- Run it as it is. The 95% intervals of the EC50 and Hill slope contain the true values, and the relative potency (true value 15) comes out as 14.4 (11.6 – 18.6).
- Set `noise_sd` of `generate` to 12 and run. The intervals widen (for B's EC50, from 1.2-fold to 1.7-fold).
- Set `top_um` of `generate` to 1 and run. B's curve never levels off, the interval's width exceeds 1000-fold, `bootstrap` warns "the concentration range does not bracket the EC50", and `compare` says "no difference in potency can be claimed".
- Reduce `concentrations` or `replicates`. Change `n_boot` of `bootstrap` and rerun just that Card.

### Molecular properties (`molecules`)

From SMILES, which writes a molecule as a string, computes properties with RDKit and lays out structural formulas, Lipinski's Rule of Five, a map of molecular weight and logP, and how alike the structures are.
Paste a SMILES into `paste` on the Canvas and that molecule joins every Card.

```text
start → paste ─pasted→ properties ─molecules→ share ┬→ structures
                                                    ├→ lipinski ─checked→ chemspace
                                                    └→ similarity
```

| Card | Kind | What it does |
| --- | --- | --- |
| `paste` | input | Sends `name` and `smiles` of a molecule to try (dopamine by default; with `smiles` empty, only the list is used) |
| `properties` | Python | Reads `molecules` (a list of "name: SMILES") and the pasted molecule, and tables molecular weight, logP (Crippen), TPSA, HBD, HBA and rotatable bonds. A SMILES that cannot be read is skipped, with the reason |
| `structures` | Python | Draws the structural formulas in a grid, with names and molecular formulas |
| `lipinski` | Python | Checks molecular weight ≤ 500, logP ≤ 5, NH+OH ≤ 5 and N+O ≤ 10, and passes a molecule with at most `max_violations` violations |
| `chemspace` | Python (Plotly) | A scatter plot of molecular weight and logP. Hover to see the name, molecular formula, TPSA and the rules violated |
| `similarity` | Python | A heat map of the Tanimoto coefficients of Morgan fingerprints (radius `radius`) |

**Try**
- Run it as it is. A table of the properties of 15 molecules and their structural formulas appear. Only atorvastatin and cyclosporin A fail the Rule of Five, and the most alike are morphine and codeine (Tanimoto 0.76).
- Paste another SMILES into `smiles` of `paste` and run.
- Set `smiles` of `paste` to `C1CC(` or `C(C)(C)(C)(C)C` and run. It does not crash: the reason it cannot be read appears above `properties`' table, and the other molecules are computed.
- Set `max_violations` of `lipinski` to 0 and rerun just that Card. Cholesterol fails.
- Set `radius` of `similarity` to 0 and rerun just that Card. The distinctions become coarse, and the average similarity rises.

### Counting cells in images (`cell-count`)

Makes synthetic fluorescence-like images whose centres are known, counts the cells with thresholding and watershed, and scores the counts.
You see what separating touching cells gains, and from where counting fails. The lower row applies the same procedure to a real image of nuclei (with no ground truth, it is not scored).

```text
start → begin ┬→ generate ─scene→ share ┬→ preprocess ─cleaned→ segment ─labels→ score
              │                         ├──────────────── scene ───────────────→ score
              │                         └→ sweep
              └→ mitosis ─image→ real
```

| Card | Kind | What it does |
| --- | --- | --- |
| `generate` | Python | Places `cells` cells with random numbers from `seed` (`touching_pairs` pairs touch). Adds uneven illumination `unevenness` and noise `noise` |
| `preprocess` | Python | Estimates and subtracts the background with a greyscale opening, and applies a Gaussian blur |
| `segment` | Python | Finds the foreground with Otsu's threshold and numbers its regions (threshold only), then separates them with watershed seeded at maxima of the distance transform |
| `score` | Python | Matches centroids one to one with the true centres and gives the count error, precision, recall and F1. Overlays what was found, what was extra and what was missed, for both methods |
| `sweep` | Python | Remakes images with other numbers of cells and noise levels, and tables the same procedure's F1 |
| `mitosis` | bash | Passes a fluorescence image of the nuclei of cultured human cells, pinned by its Nix hash (Moffat et al., Cell 2006, CC0; from scikit-image's data set; fetched once at Nix build time, no network while running) |
| `real` | Python | Applies the same procedure to the real image and shows the counts and outlines |

**Try**
- Run it as it is. Against a true count of 80, threshold only finds 60 (recall 0.75) and watershed 74 (recall 0.93). On the real image of nuclei, threshold only finds 271 and watershed 311 (with no ground truth, check the outlines by eye).
- Set `cells` of `generate` to 320 and run. Threshold only's recall falls to 0.24, while watershed keeps 0.79.
- Raise `noise` of `generate`. As the top left of `sweep`'s table shows, the fewer the cells, the more the threshold falls into the noise and overcounts.
- Change `min_distance` of `segment` and rerun just that Card. Larger values separate less.

### Survival analysis (`survival`)

With `lung`, bundled with the survival package (real data from the NCCTG advanced lung cancer trial, 228 patients; Loprinzi et al., J Clin Oncol 1994), finds Kaplan–Meier curves, the log-rank test and a Cox model.
How much the same hazard ratio moves with "who is included" and "what is adjusted for" is laid out in one figure on the Canvas. These are associations in old trial data; they show no cause and are not advice.

```text
start → load ─lung→ share ─┬→ cohort ─cohort→ split ─┬→ km
                           │                         └→ cox ─fit→ zph
                           └→ specs
```

| Card | Kind | What it does |
| --- | --- | --- |
| `load` | R | Adds `event` and `female` columns to `lung`, writes it as CSV, and tables each variable's median, range and missing values |
| `cohort` | R | Chooses subjects by `age_min`, `age_max` and `ecog_max`. Shows the numbers of people and deaths next to the whole |
| `km` | R | Kaplan–Meier curves split by `group` (sex / ecog / age), medians and the log-rank test. Keeps the curves' values in `curves.csv` |
| `cox` | R | Fits a Cox model with the variables in `covariates` and tables hazard ratios with 95% confidence intervals |
| `zph` | R | Plots the scaled Schoenfeld residuals of `cox.zph` and shows p-values per variable and overall |
| `specs` | R | Finds the hazard ratio of `exposure` for every combination of `subsets` × `adjustments` and lays them out like a forest plot |

**Try**
- Run it as it is. Median survival is 426 days for women and 270 for men, and the log-rank χ² is 10.33. `specs` lays out the hazard ratios of 24 combinations of subjects and adjustments.
- Read the figure in `specs`. The hazard ratio for women is 0.59 for everyone, but 0.76 (with an interval crossing 1) under 65 adjusted up to ECOG, and 0.39 for ECOG 2 and above adjusted for four variables. In the latter, missing weight-loss values reduce 51 people to 46.
- Set `group` of `km` to `ecog` and run. The log-rank χ² changes from 10.33 to 18.98.
- Add `ph.karno` to `covariates` of `cox` and run from `cox` onwards. The overall p-value of `zph` falls from 0.216 to 0.016, and the proportional hazards assumption becomes doubtful (in a single-Card run `zph`'s input is not updated, so run on from `cox`).
- Set `age_min` of `cohort` to 65, run, and look at how `km` and `cox` change.

### Meta-analysis: the BCG vaccine (`meta-analysis`)

With `dat.bcg`, bundled with metafor (13 trials of the BCG vaccine against tuberculosis; Colditz et al., JAMA 1994), compares fixed-effect and random-effects pooling, heterogeneity and a meta-regression on latitude.
How much the conclusion moves with choices such as leaving one trial out or changing the estimator can be seen on the Canvas. This is an example of statistical method, not advice about vaccination.

```text
start → trials ─trials→ share ─┬→ pool
                               ├→ funnel
                               ├→ moderator
                               └→ sensitivity
```

| Card | Kind | What it does |
| --- | --- | --- |
| `trials` | R | Finds each trial's log risk ratio and variance from the counts (`escalc`), as a table and a CSV |
| `pool` | R | Pools with fixed effect and random effects (`method`), and shows a forest plot with both diamonds, and I², τ² and Q. Trial numbers written in `exclude` are left out |
| `funnel` | R | A funnel plot with the regression test and the result of trim and fill |
| `moderator` | R | Runs a meta-regression on `moderator` (by default the absolute latitude) and shows the slope, the residual τ² and the proportion explained in a figure |
| `sensitivity` | R | Lays out the pooled risk ratio when each trial is left out in turn (fixed effect and DL), and with other estimators of τ² |

**Try**
- In the forest plot of `pool`, compare the fixed-effect diamond, 0.65, with the random-effects diamond, 0.49. I² is 92.1%.
- Set `exclude` of `pool` to `[8]` (TPT Madras, the largest trial) and run only `pool`. The fixed effect moves from 0.65 to 0.48.
- Set `method` of `pool` to `REML`. τ² changes from 0.309 to 0.313, but the pooled risk ratio hardly moves.
- In `moderator`, see latitude reduce τ² from 0.313 to 0.076. Set `moderator` to `year` to compare.

### RNA-seq: airway and dexamethasone (`rna-seq`)

With Bioconductor's airway (real data from four airway smooth muscle cell lines treated with dexamethasone; Himes et al., PLoS One 2014, GEO GSE52778), finds differentially expressed genes with DESeq2 and edgeR.
How much "the number of differentially expressed genes" changes with thresholds and design can be checked in each Card. This is a cell-culture experiment; it shows no effect of treatment and is not medical advice.

```text
start → load ─airway→ filter ─counts→ share ─┬→ normalize ─normalized→ pca
                                             ├→ deseq ─results→ fan ─┬→ volcano
                                             │                       ├→ thresholds
                                             │                       ├→ top
                                             │                       ├→ compare ←─results─ edger
                                             │                       └→ design
                                             ├→ edger
                                             └→ design
```

| Card | Kind | What it does |
| --- | --- | --- |
| `load` | R | Writes airway's count matrix (63,677 genes × 8 samples) and gene names as CSV, and tables the samples |
| `filter` | R | Removes genes whose total reads are below `min_total`, and shows the distribution and the cut-off in a figure |
| `normalize` | R | Finds size factors by median of ratios and compares the distributions before and after normalisation. Passes the vst on for the PCA |
| `pca` | R | Runs a principal component analysis on the `top_genes` most variable genes, coloured by treatment and cell line |
| `deseq` | R | Chooses `~ cell + dex` or `~ dex` by `paired`, writes DESeq2's results as CSV and draws an MA plot |
| `volcano` | R | Counts by the `fdr` and `lfc` thresholds and draws a volcano plot, naming the genes in `known` |
| `thresholds` | R | Tables the number of genes for each point of a grid of FDR and log2FC |
| `top` | R | Tables the top `top` genes by FDR, with the ranks of the known responsive genes in `known` |
| `edger` | R | Tests the same counts with edgeR's quasi-likelihood F test |
| `compare` | R | Overlays the log2FC of DESeq2 and edgeR, and counts the genes found by both and by only one |
| `design` | R | Also runs DESeq2 with `~ dex`, and lays its estimates and statistics next to those of `~ cell + dex` |

**Try**
- Run it as it is (the first time, the Nix builds of DESeq2, edgeR and airway take a while). `compare` shows 3985 genes on which DESeq2 and edgeR agree.
- Read the figure in `design`. Including the cell line term increases the genes with FDR < 0.05 from 2713 to 4030. The log2FC hardly changes; only the statistics grow.
- Change `lfc` of `volcano` from 1 to 2, or change `fdr`, and run only `volcano`. The counts match those in `thresholds`' table.
- Set `min_total` of `filter` to 100 and run from `filter` onwards.
- In `top`, look at the ranks of FKBP5, DUSP1 and TSC22D3. Ranking genes by FDR and by the size of their fold change do not give the same order (FKBP5 is 16.5-fold at rank 143; DUSP1 is 7.7-fold at rank 3).

### A correlation that reverses when stratified (`simpson`)

A relationship seen in the whole can reverse its sign when split into strata. This is checked on two real datasets: the bill length and depth of penguins (palmerpenguins, Gorman et al. 2014) and applications to Berkeley in 1973 (R's `UCBAdmissions`).
A note says that which value to read is not decided by the data alone, but by the causal question.

```text
start → branches ─┬→ penguins ─penguins→ share ─┬→ correlate ─points, correlations→ figure
                  │                             └→ survey
                  └→ admissions ─admissions→ applications
```

| Card | Kind | What it does |
| --- | --- | --- |
| `penguins` | R | Writes the 342 birds with both bill length and depth as CSV, and tables the counts by species and island |
| `correlate` | R | Splits by `stratify` (species / sex / island) and tables r, its confidence interval and the slope for the whole and each stratum |
| `figure` | R | Overlays on the same points the regression line for the whole (dashed) and the lines for each stratum |
| `survey` | R | Lays out r for the three stratifications in one figure, showing where it reverses |
| `admissions` | R | Tables the admission rates and odds ratios per department and overall, and the Mantel–Haenszel common odds ratio |
| `applications` | R | Plots admission rates as points sized by the number of applicants, with the overall rate as a dashed line |

**Try**
- Run it as it is. The r of bill length and depth is −0.24 overall, but 0.39, 0.65 and 0.64 within species: the direction reverses.
- Set `stratify` of `correlate` to `sex` and run from `correlate` onwards. r stays negative in every stratum and does not reverse.
- Set `stratify` to `island`. It stays negative on Biscoe, and turns positive on Dream and Torgersen.
- In `admissions`' table, see the overall odds ratio of 1.84 become 0.90 when adjusted for department. In `applications`' figure, see how applications were skewed.

### Scheduling a battery (`battery`)

```text
start → settings → market ─ share ─┬→ schedule
                                   ├→ capacities
                                   ├→ tariffs
                                   └→ check
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends the capacity `capacity_kwh` (10), the power limit `power_kw` (3), the round-trip efficiency `round_trip_efficiency` (0.9), the export ratio `export_ratio` (0.5), the step of stored energy `soc_step_kwh` (0.1), `seed` (7) and `days` (7) |
| `market` | Julia | From the seed, makes a house's consumption, solar generation and three tariffs (flat, time-of-use, market-linked) per hour. States in the data and figures that all of it is made up |
| `schedule` | Julia | Finds the charging and discharging with the lowest electricity bill by dynamic programming with the stored energy as the state. Shows the price, charging and discharging, and stored energy under the `tariff` tariff |
| `capacities` | Julia | Solves for other capacities and shows the saving, the saving per additional kWh, and how operation differs by capacity |
| `tariffs` | Julia | Runs the same battery under four contracts and compares the saving, the number of cycles and how the stored energy moves |
| `check` | Julia | Compares with exhaustive search over a short period and a small battery, checks that at a flat price with exports at the same price it does not run, and tables the difference with a finer step. Fails if anything is off |

**Try**
- Run it as it is. Over the made-up week, with exports at half price, it saves about 786 yen even at the flat tariff, about 1478 yen with time-of-use and about 1442 yen market-linked. `check` shows dynamic programming agreeing with exhaustive search.
- Change `tariff` of `schedule` to `flat`, `tou` or `spot` and rerun just that Card.
- Lower `round_trip_efficiency` of `settings` and run. With time-of-use prices, once the efficiency falls below cheap / expensive (18 / 42), the round trip no longer pays.
- Set `export_ratio` of `settings` to 1 and exports earn the same as imports cost, so storing solar is pointless. At the flat tariff the saving is 0 yen with 0 cycles: the battery does not run.
- Change `capacities_kwh` of `capacities` and see where it levels off.

### Fluctuations in chemical reactions (`gillespie`)

```text
start → settings ─ share ─┬→ trajectories
                          ├→ stationary ─┐
                          ├→ scaling ────┼→ check
                          └→ binding ────┘
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends the synthesis rate `synthesis` (2), the degradation rate `degradation` (0.1), the volume `volume` (1), the number of runs `runs` (2000) and `seed` (1) |
| `trajectories` | Julia | Computes `runs` runs with Gillespie's method and overlays `shown_runs` trajectories, the mean of all runs and the solution of the rate equation |
| `stationary` | Julia | Compares the distribution of end points after enough time with the theoretical Poisson distribution. Passes on the mean, variance and Fano factor |
| `scaling` | Julia | Measures the coefficient of variation at other volumes and compares it with the theoretical 1/√N. Also shows the trajectories converted to concentrations, per volume |
| `binding` | Julia | Computes A + B ⇌ C at other volumes and compares the fraction bound with the exact solution (detailed balance) and the rate equation |
| `check` | Julia | Compares the three results with closed forms and tables how many standard errors off they are. Fails beyond `limit_sigma` |

**Try**
- Run it as it is. The trajectories fluctuate, but the mean of 2000 runs follows the rate equation. The stationary mean and variance are close to the theoretical 20 (20.15, 20.46), and the Fano factor is 1.015. With two of each, A + B ⇌ C has a fraction bound of 0.57, off from the rate equation's 0.50 (it approaches it as the volume grows).
- Set `volume` of `settings` to 0.1 and run. It becomes a Poisson distribution with mean 2, and trajectories drop to 0 too. The Fano factor stays 1.
- Increase `shown_runs` of `trajectories` and rerun just that Card.
- Change `on_rate` or `concentration` of `binding` and see how far it departs from the rate equation.
- Make `limit_sigma` of `check` smaller and see how far statistical fluctuation takes it off.

### An inverse problem in heat conduction (`heat-inverse`)

```text
start → settings → forward ─ share ─┬→ spectrum
                                    ├→ naive
                                    ├→ tikhonov ─choice─┐
                                    ├────────────────── reconstruct
                                    ├→ horizon
                                    └→ check
```

| Card | Kind | What it does |
| --- | --- | --- |
| `settings` | input | Sends the grid `cells` (100), the thermal diffusivity `alpha` (1), the measurement time `final_time` (0.002), the noise `noise` (0.2 °C) and `seed` (3) |
| `forward` | Julia | Makes a made-up initial temperature, computes the temperature at time T by eigendecomposition of the finite-difference matrix with insulated ends, and adds noise. Checks that the total heat is conserved |
| `spectrum` | Julia | Shows how the singular values decay at each time, and the index of the mode where the measurement's components sink into the noise |
| `naive` | Julia | Shows that multiplying by the inverse matrix as it is diverges (even without noise, by rounding error) |
| `tikhonov` | Julia | Sweeps λ and shows the L-curve and the error. Compares the discrepancy principle, the corner of the L and the λ closest to the truth |
| `reconstruct` | Julia | Lays the recovery with the chosen λ (the discrepancy principle's value when `lambda` is 0), and with too small and too large a λ, next to the truth |
| `horizon` | Julia | Recovers from the same noise at other measurement times, showing that the later it is, the more detail is lost |
| `check` | Julia | Tables checks of the conservation of total heat, the analytic solution of a cosine mode, agreement with an implicit scheme, recovery without noise at an early time, and the maximum principle. Fails if anything is off |

**Try**
- Run it as it is. The naive inverse diverges to as much as 3.8e+33 °C, while Tikhonov regularisation recovers the shape with an error of 5.1 °C. In `horizon`, the later the measurement, the fewer modes stay above the noise (from 37 to 3), and the error grows from 1.9 °C to 10.6 °C.
- Set `final_time` of `settings` to 0.02 and run. With the same noise, the recovery's error grows.
- Change `lambda` of `reconstruct` and rerun just that Card. Back at 0, it uses the discrepancy principle's value.
- Set `noise` of `settings` to 0 and `final_time` to 0.00005, and the recovery returns to the truth.
- Change `times` of `horizon` and find the time at which the jaggedness disappears.

### Comparing ecological models (`ecology-models`)

```text
start → load ─ share ─┬→ fit ─ fits-share ─┬→ check
                      │                    ├→ phase
                      │                    └→ scores
                      └→ forecast ────────────→ scores
```

| Card | Kind | What it does |
| --- | --- | --- |
| `load` | Julia | Reads the Hudson's Bay Company fur records (thousands of pelts, 1900–1920) from a CSV bundled with the Card, and shows them with the source |
| `fit` | Julia | Minimises the squared error of logs with Nelder–Mead while solving three models with RK4 (in log space, from `restarts` starting points, up to the year `fit_until`). Finds AIC, AICc and BIC and overlays the fits on the records |
| `forecast` | Julia | Fits up to `fit_until` (1914) and compares the errors in forecasting the years held out |
| `scores` | Julia | Tables log RMSE, AIC, AICc, BIC, forecast error, the number of starting points that reached the same solution, and the best values |
| `phase` | Julia | Extends each model's best solution `years` years ahead and compares them in a phase diagram |
| `check` | Julia | Checks RK4 against the Lotka–Volterra conserved quantity, and compares the fitted values with the posterior of the Stan case study |

**Try**
- Run it as it is. Over these 21 years the three models fit almost equally (log RMSE 0.219), AICc favours Lotka–Volterra with the fewest parameters, and Holling's handling time h goes towards 0. The error of forecasts fitted up to 1914 is also smaller for Lotka–Volterra (0.353, against 0.421 for the others). The sticky note says that a good AIC does not mean the mechanism is right, and that pelts are not population counts.
- Change `fit_until` of `forecast` between 1908 and 1918 and rerun just that Card. The more years held out, the harder the forecast.
- Lower or raise `restarts` of `fit` and look at "starting points that reached the same solution" and the errors in `scores`.
- Increase `years` of `phase` and see where the logistic spiral heads.

## Draft versions (for the Agent to implement)

`casa-draft`, `gw150914-draft`, `protein-6lu7-draft`, `exoplanet-transit-draft` and `seismic-trench-draft` are projects that hold the same plans as the demos above, as Drafts only (`casa-draft` is the plan up to the RGB image, without CIAO and `spectrum`).
Each has a start Card and three Drafts, and each Draft's description says what it receives, what it does and what it passes on (almost the same text as the descriptions of the implemented demos' Cards).

Press "Generate Cards" at the top right of the Canvas and the Agent makes Cards from the Drafts, runs them and checks ([Generating Cards from Drafts](features.md#generating-cards-from-drafts)).

## Dynamical systems gallery (`dynamics-draft`)

A demo in which the Agent splits six independent Drafts among helpers and implements them at the same time.

```text
start → settings ─ share ─┬→ lorenz      the Lorenz attractor (RK4, density in the x–z plane)
                          ├→ mandelbrot  the Mandelbrot set (smooth colouring)
                          ├→ logistic    the bifurcation diagram of the logistic map
                          ├→ life        the Game of Life (200 generations on a torus)
                          ├→ grayscott   Gray–Scott reaction–diffusion patterns
                          └→ pendulum    a chaotic double pendulum (two runs with initial angles 1e−6 apart)
```

`settings` only hands the image size (`size` 480) and the random seed (`seed` 7) to the six Drafts; the Drafts are not connected to each other.
Each Draft's description gives the steps of the computation and `assert`s that check the result is right (a glider in the Game of Life moving one cell diagonally in four generations, the double pendulum's energy error being small, and so on).

Press "Generate Cards" and the Agent splits the six Drafts among helpers, and a thrush of a different colour for each helper writes and checks its own Card at the same time ([Splitting the work](features.md#splitting-the-work-delegate)).

## MNIST · comparing classifiers (Draft) (`mnist-classifiers-draft`)

A demo that has the Agent implement the same problem as [MNIST · comparing classifiers](#mnist--comparing-classifiers-mnist-classifiers). Unlike the dynamical systems gallery, the helpers' work merges into one comparison at the end.

```text
start → load → prepare → share ─┬→ knn ──────┐
          (implemented)         ├→ logistic ─┤
                                ├→ svm ──────┤
                                ├→ forest ───┤
                                ├→ boosting ─┼→ compare
                                ├→ mlp ──────┤
                                ├→ cnn ──────┤
                                └────────────┘
```

| Card | State | Who writes it |
| --- | --- | --- |
| `load`, `prepare` | Implemented. Pins the real MNIST by its Nix hash and passes on a stratified 5000 training and 1000 test images | There from the start |
| `knn`, `logistic`, `svm`, `forest`, `boosting`, `mlp`, `cnn` | Drafts. No wires between them, and separate environments (scikit-learn, xgboost, torch) | Helpers, one each, at the same time |
| `compare` | A Draft. Waits for the seven results and makes a ranking and a scatter plot | The Agent, after the helpers report |

- **Why they can merge:** each Draft's description writes out the type of the output `result` (`mnist.Result`). This shared agreement lets helpers implement independently and lets `compare` read their outputs with one type.
- **What is checked:** each Draft's description has the steps of the computation and `assert`s. Besides the counts, the 0–9 range and a minimum accuracy, they check properties that each method's result must have (kNN agreeing with brute force in numpy, logistic regression's predictions agreeing with the weighted sums, the forest's predictions agreeing with the average of its trees, formulas for the networks' parameter counts and so on). `compare` checks that the digests agree and that the accuracies agree with values recomputed from the predictions.
- **Why the counts are small:** 5000 training and 1000 test images keep verification runs short, compared with the finished version's 10000 and 2000. A project can run only one run at a time; coordinate through the helper instructions so the parent runs after the helpers finish writing.
- **The answers:** neither this demo nor the finished `mnist-classifiers` appears in the Agent's `reference_read` list or can be read with it.

## csharp-calibration

The **Cross-language calibration (C#)** demo passes three C++ readings (1.25, 2.5, 3.75) through a C# Card with `factor = 1.6`, then summarizes the typed result in Python and retains a CSV. The expected calibrated values are 2, 4 and 6: sum 12 and mean 4. Change `calibrate.factor` to 2 and rerun; the sum becomes 15 and mean 5. The C# log uses a fixed-hash Newtonsoft.Json dependency. Each Card's source and the saved CSV remain available to inspect. See [C# Cards](CSharp.md) for the SDK and worker lifetime.

## Visualization → report (`visualization`)

A typed aggregate fans out to line, scatter and histogram Cards, then a Python report collects their saved PNG/SVG/PDF figures and plotted values. Change `aggregate.offset` to produce different figures with the same data size. The [Visualization guide](Visualization.md) describes the three library templates and their parameters.

## marimo notebook → typed result (`marimo`)

The upstream Card scales a measurement of 7 by its `factor` of 2. A marimo notebook visualizes the original and scaled values, and a Bash Card logs and displays the selected result of 14. The notebook invocation retains static HTML, a figure, source cells and binding and result metadata. Set the upstream `factor` to 3 and rerun it to save a result of 21. See [marimo Cards](Marimo.md).
