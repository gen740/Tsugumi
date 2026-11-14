# The bundled demos

What Cards each bundled demo connects, how, and what happens when it runs.
Choose "Open a Copy" from "Browse Demos" on the home screen and an editable copy is made in `~/Documents/Tsugumi/<demo>` (the bundled original is not changed).
The demos come as a Japanese set and an English set (`demos/ja`, `demos/en`), the same demos with their text in one language; the app shows the set of its language. Choose the language in settings; inspect values in each demo's own run records.
Operations on screen are in [Usage](Usage.md); kinds of Card and the rules of execution are in [Features](features.md#2-cards-and-workflows).

| Demo | Subject | What it shows |
| --- | --- | --- |
| [Data to report](#data-to-report-report) | Aggregating a sales table into a report | Reproduction by seed, failure and `blocked`, rerunning one Card with saved inputs, artifacts, child Workflows |
| [Collatz (C++ / Python / Julia / R)](#collatz-c--python--julia--r-collatz) | The Collatz sequence | Cross-checking independent implementations in four languages, branches, merges and loops, `maxRuns`, detecting integer overflow |
| [Python → Ruby → C++](#python--ruby--c-ruby-pipeline) | Passing a typed integer across languages | Ruby Hash outputs, parameters, resident run-start state, live displays |
| [Live Stream](#live-stream-live-stream) | Plotting a random walk as it goes | A stream made by a feedback loop, stopping at once, WebGPU line charts |
| [Plots](#plots-plots) | Diffusion of random walks | Matplotlib figures and `FuncAnimation` published through Report; Plotly and Altair become interactive charts |
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

The arrows in the diagrams are a Card's wires (`output port → input port`). The kinds of Card are trigger (where a run starts), input (an input whose value is edited on the Canvas), fork (distribution), branch (conditional branching), merge, subgraph (Card in Card) and code Cards ([supported languages](CardGuide.md#inputs-results-and-reports)).
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

**Try**
- Change `seed` or `heading` and run. With the same `seed` the aggregate is the same and only the heading changes.
- Turn `fail_on_purpose` of `aggregate` On and run. `aggregate`'s log gives the reason, and `render` downstream is `blocked`. Turn it Off and rerun only `aggregate`: with the saved inputs it succeeds at once.
- Double-click `archive` to go into the child Workflow, and preview and export the artifact's directory. Artifacts remain even after the run cache is cleared.

### Collatz (C++ / Python / Julia / R) (`collatz`)

The same Collatz iteration implemented separately in C++, Python, Julia and R, checking that the four sequences agree.
The iteration itself is drawn with the Cards' wires, so the behaviour of branches, merges and loops can be followed on the Canvas.

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

**Try**
- Run it and stop (Cancel) in the middle. It stops at once, and the chart up to that point stays.
- Change `interval_ms`, `seed`, `samples` or `window` of `plot` and run again. The series resets, and with parallelism 1 or 2 no value goes missing or out of order.

### Plots (`plots`)

Makes random walks and draws them with Matplotlib, Plotly and Altair. `report.figure()` publishes Matplotlib figures and plays animations; `report.figure(fig)` publishes a Plotly figure or an Altair chart as an interactive display.

**Try**
- Run it: `diffusion`'s display plays as an animation, and the last frame stays.
- Enlarge the displays of `explore` and `spread`, and try zooming, hovering (values per walker) and the legend.
- Change `walkers`, `steps`, `seed` or `frames` and run again.

### Cas A · Chandra RGB (`casa`)

Pins with Nix the X-ray event data of the supernova remnant Cassiopeia A (Chandra) and the analysis tool CIAO, and splits the photons into three energy bands for an RGB image.
It also draws the energy spectrum of the same photons, to check which element's lines became which colour.

**Try**
- Run it as it is. The default observation is ObsID 25628 (2.1 ks, 13 MB). Read the photon-count table in `bands` and compare it with the RGB image.
- Change `obsid`, `file` and `hash` of `load` to ObsID 114 (2000, 50 ks, 520 MB) or ObsID 19606 (2019, 49 ks, 349 MB): the deeper observations give a sharper image, and compare the observations (the download settings are on the sticky note and in `load`'s description). Only the first run takes time to fetch.
- Change the bands of `bands`, the stretch of `rgb`, or the range and binning of `spectrum`, and rerun just that Card. The spectrum shows that the green band contains exactly the Si line, and everything from S up falls in blue.
- Open the version tree. It holds the process of trying bands, grids and stretches: failed branches, branches not adopted, the adopted version `rgb-v1`, the version that added the spectrum on top, and the version that pinned CIAO ([Versions](Versions.md#demo-histories)).

## Science demos (fetching and analysing public data)

Four demos with the same shape: `load` fetches data from its publisher, and the two Cards that follow analyse it into figures and tables.

`load` is a Python Card allowed the network (`network: true`). It fetches the files of `urls` and `names`, passes them on as a folder, and shows the size and SHA-256 of each.
Where Cas A's `load` pins the fetch with Nix, these fetch on every run.

| Demo | Source | What to try and check |
| --- | --- | --- |
| **GW150914** (`gw150914`) | GWOSC's H1/L1 strain data (4 kHz, 32 s HDF5) | Change `f_low` / `f_high` (35–350 Hz); inspect the waveform, H1/L1 overlap and chirp spectrogram. SNR is a rough estimate |
| **6LU7** (`protein-6lu7`) | RCSB PDB's SARS-CoV-2 main protease with inhibitor N3 | Change `cutoff` (4 Å); inspect the highlighted pocket and whether catalytic His41 and Cys145 are included |
| **Kepler-6 b** (`exoplanet-transit`) | MAST quarter-3 light curve of KIC 10874614 (FITS) | Change `window_days`; inspect the folded dip (about 3.23 days, 1% depth) and radius ratio Rp/R* = √δ |
| **Japan Trench** (`seismic-trench`) | USGS M4+ hypocentres, 2000–2024, 34–42°N, 138–146°E (CSV) | Change latitude limits and `mc`; compare the depth section and Gutenberg–Richter b-value |

**Try**: run it as it is, then change a parameter of an analysis Card (the pass band, `cutoff`, `window_days`, the band of latitude, `mc` and so on) and rerun just that Card. The analysis alone is redone with the saved inputs, without fetching again.

## Demos that compute locally

Demos that use no network while running. Their data is synthesised, comes bundled with a package, or is pinned with Nix (fetched once, at build time).

### MNIST · comparing classifiers (`mnist-classifiers`)

Each classifier Card shows its accuracy, single-thread CPU time (training and prediction) and a breakdown of its mistakes, and passes its predictions for all test images to `compare` as a typed message (`Result`).

**Try**

- Run it as it is. The seven branches run at the same time (how many at once is "Parallel runs" in the settings, default 4). Inspect accuracy, errors and the uncertainty of pairwise differences in `compare`.
- Set `max_depth` of `forest` to 3 and run just that Card. No other branch runs; inspect how restricting tree depth changes its errors.
- To compare again, run from `forest` onwards ("Run from Here Downstream"). Only `forest` and what follows `compare` run, and `compare` compares the new `forest` result with the saved inputs of the other branches and `share`. The branches you did not change do not run, so their accuracy is the same.
- Change `k` of `knn`, `gamma` of `svm` or `epochs` of `cnn`.
- Increase `train_size` of `prepare` and see which classifiers benefit from more images and whose time grows (`svm`'s and `knn`'s prediction times grow).

The data is MNIST by LeCun, Cortes and Burges, fetched from PyTorch's mirror `https://ossci-datasets.s3.amazonaws.com/mnist/`.

### Newton's method (`newton`)

Solves $z^3 - 1 = 0$ on the complex plane to see how the starting point decides where it goes and how fast it converges. Alongside estimates of quadratic convergence and a map of where each point converges, it shows cases that lose quadratic convergence and cases that do not converge.

**Try**
- Run it as it is. The errors in `orbits` fall quadratically on the log axis, and the estimated order in `order` lines up with the theory. `basins` draws the fractal boundaries between where points converge.
- Set `center_re` of `basins` to -0.8 and `half_width` to 0.2 and rerun just that Card. Wherever you zoom on the boundary, all three colours gather.
- Set `offset` of `trouble` to 1e-5. The first step is thrown out to $1/(3 z_0^2)$, and far away it comes back only by 2/3 per step, so inspect the iteration count and raise `max_iter` if convergence reaches its cap.
- Set `damping` of `settings` to 0.5 and run the whole. From every starting point the order becomes 1 with ratio 0.5, and the shape of the boundaries on the map changes too.
- Set `coeffs` of `settings` to `[1, 0, -3, 2]` ($(z-1)^2(z+2)$). At the root of multiplicity 2 the order drops to 1 with ratio 0.5, and rounding limits the final error.
- Set `coeffs` of `settings` to `[1, 0, -2, 2]` ($z^3 - 2z + 2$). A black region appears around 0 (starting points drawn into a cycle of period 2 that never converge).

### Pharmacokinetics (`pharmacokinetics`)

Compares how concentration moves with the dosing interval, in a one-compartment oral model of a made-up drug, "Tsugumicin". **This is a made-up model for teaching. Its values are not those of any real drug, and it is neither medical advice nor dosing guidance.** The therapeutic range is invented too.

**Try**
- Run it as it is and look at `window`. 400 mg every 24 hours and 200 mg every 12 hours give the same mean concentration (the table in `check`), and shortening the interval narrows the gap between peak and trough.
- Set `missed_count` of `missed` to 2 and rerun just that Card. For the same daily dose, the longer the interval, the more the time below the range grows.
- Set `half_life_h` of `settings` to 16 and run the whole. The accumulation ratio grows, and so does the time to reach steady state.
- Shorten `days` of `settings` and "reaching steady state" in `check` shows that it does not get there.

### Maximum flow and minimum cut (`max-flow`)

On a made-up layered network made from a seed, checks that the maximum flow equals the minimum cut. You can compare which edges increase the maximum flow when widened, and which do not.

**Try**
- Run it as it is and look at `sweep`. Of the cut's three edges only two increase it; `a2->b2` does not, because another minimum cut remains (`verify` counts two minimum cuts).
- Set `edge` of `whatif` to `s->a2` and `raise_by` to 1000. Even an edge running at full capacity does not increase the maximum flow if it is off the cut.
- Set `edge` of `whatif` to `b3->c3`. Raising it by 10 increases the flow by only 8, because the next cut gets in the way; increasing `raise_by` further gains nothing beyond that.
- Change `seed` of `settings` and run the whole. On another network, the values in `verify` still all agree.

### Dose–response curve (`dose-response`)

Makes plates of made-up compounds A and B with random numbers (synthetic data), fits a four-parameter logistic (Hill curve), and puts confidence intervals on the EC50 and the Hill slope.
The true values are known, so you can check on the spot whether they were recovered. These are not real drugs, and this is not medical advice.

**Try**
- Run it as it is. Check whether the EC50 and Hill-slope intervals contain their known values, then compare relative potency with the synthetic truth.
- Set `noise_sd` of `generate` to 12 and run. Compare the uncertainty around each compound's EC50.
- Set `top_um` of `generate` to 1 and run. Inspect B's unbracketed curve and the uncertainty warnings in `bootstrap` and `compare`.
- Reduce `concentrations` or `replicates`. Change `n_boot` of `bootstrap` and rerun just that Card.

### Molecular properties (`molecules`)

From SMILES, which writes a molecule as a string, computes properties with RDKit and lays out structural formulas, Lipinski's Rule of Five, a map of molecular weight and logP, and how alike the structures are.
Paste a SMILES into `paste` on the Canvas and that molecule joins every Card.

**Try**
- Run it as it is. A table of the properties of 15 molecules and their structural formulas appear. Inspect the Rule of Five violations and the pairwise similarity scores.
- Paste another SMILES into `smiles` of `paste` and run.
- Set `smiles` of `paste` to `C1CC(` or `C(C)(C)(C)(C)C` and run. It does not crash: the reason it cannot be read appears above `properties`' table, and the other molecules are computed.
- Set `max_violations` of `lipinski` to 0 and rerun just that Card. Cholesterol fails.
- Set `radius` of `similarity` to 0 and rerun just that Card. The distinctions become coarse, and the average similarity rises.

### Counting cells in images (`cell-count`)

Makes synthetic fluorescence-like images whose centres are known, counts the cells with thresholding and watershed, and scores the counts.
You see what separating touching cells gains, and from where counting fails. The lower row applies the same procedure to a real image of nuclei (with no ground truth, it is not scored).

**Try**
- Run it as it is. Compare thresholding and watershed against the known synthetic count. On the real nuclei image, check the outlines by eye because there is no ground truth.
- Set `cells` of `generate` to 320 and run. Compare each method's recall as cells overlap more.
- Raise `noise` of `generate`. As the top left of `sweep`'s table shows, the fewer the cells, the more the threshold falls into the noise and overcounts.
- Change `min_distance` of `segment` and rerun just that Card. Larger values separate less.

### Survival analysis (`survival`)

With `lung`, bundled with the survival package (real data from the NCCTG advanced lung cancer trial, 228 patients; Loprinzi et al., J Clin Oncol 1994), finds Kaplan–Meier curves, the log-rank test and a Cox model.
How much the same hazard ratio moves with "who is included" and "what is adjusted for" is laid out in one figure on the Canvas. These are associations in old trial data; they show no cause and are not advice.

**Try**
- Run it as it is. Read the survival curves and log-rank test, then inspect the combinations of subjects and adjustments in `specs`.
- Read the figure in `specs`. Compare the hazard ratio and confidence interval across age groups and adjustment sets. Check how missing covariates change the number of included subjects.
- Set `group` of `km` to `ecog` and run. Compare the group curves and log-rank test.
- Add `ph.karno` to `covariates` of `cox` and run from `cox` onwards. Inspect `zph` to judge the proportional hazards assumption (a single-Card run does not update `zph`'s input, so run downstream from `cox`).
- Set `age_min` of `cohort` to 65, run, and look at how `km` and `cox` change.

### Meta-analysis: the BCG vaccine (`meta-analysis`)

With `dat.bcg`, bundled with metafor (13 trials of the BCG vaccine against tuberculosis; Colditz et al., JAMA 1994), compares fixed-effect and random-effects pooling, heterogeneity and a meta-regression on latitude.
How much the conclusion moves with choices such as leaving one trial out or changing the estimator can be seen on the Canvas. This is an example of statistical method, not advice about vaccination.

**Try**
- In the forest plot of `pool`, compare the fixed-effect and random-effects diamonds and inspect heterogeneity.
- Set `exclude` of `pool` to `[8]` (TPT Madras, the largest trial) and run only `pool`. Compare the pooled estimate and its uncertainty.
- Set `method` of `pool` to `REML`. Compare τ² and the pooled risk ratio.
- In `moderator`, compare residual heterogeneity with latitude and with `moderator` set to `year`.

### RNA-seq: airway and dexamethasone (`rna-seq`)

With Bioconductor's airway (real data from four airway smooth muscle cell lines treated with dexamethasone; Himes et al., PLoS One 2014, GEO GSE52778), finds differentially expressed genes with DESeq2 and edgeR.
How much "the number of differentially expressed genes" changes with thresholds and design can be checked in each Card. This is a cell-culture experiment; it shows no effect of treatment and is not medical advice.

**Try**
- Run it as it is (the first time, the Nix builds of DESeq2, edgeR and airway take a while). Inspect which genes DESeq2 and edgeR agree on in `compare`.
- Read the figure in `design`. Compare gene counts, log2 fold changes and significance with and without the cell-line term.
- Change `lfc` of `volcano` from 1 to 2, or change `fdr`, and run only `volcano`. The counts match those in `thresholds`' table.
- Set `min_total` of `filter` to 100 and run from `filter` onwards.
- In `top`, look at the ranks of FKBP5, DUSP1 and TSC22D3. Compare ranking by FDR with ranking by fold-change size.

### A correlation that reverses when stratified (`simpson`)

A relationship seen in the whole can reverse its sign when split into strata. This is checked on two real datasets: the bill length and depth of penguins (palmerpenguins, Gorman et al. 2014) and applications to Berkeley in 1973 (R's `UCBAdmissions`).
A note says that which value to read is not decided by the data alone, but by the causal question.

**Try**
- Run it as it is. Compare the overall correlation of bill length and depth with the correlations within species.
- Set `stratify` of `correlate` to `sex` and run from `correlate` onwards. r stays negative in every stratum and does not reverse.
- Set `stratify` to `island`. It stays negative on Biscoe, and turns positive on Dream and Torgersen.
- Compare the overall and department-adjusted odds ratios in `admissions`, then inspect the distribution of applications in `applications`.

### Scheduling a battery (`battery`)

**Try**
- Run it as it is. Compare savings under the synthetic tariffs and inspect the agreement between dynamic programming and exhaustive search in `check`.
- Change `tariff` of `schedule` to `flat`, `tou` or `spot` and rerun just that Card.
- Lower `round_trip_efficiency` of `settings` and run. Compare savings as efficiency crosses cheap / expensive (18 / 42) under time-of-use prices.
- Set `export_ratio` of `settings` to 1 and exports earn the same as imports cost, so storing solar is pointless. At the flat tariff the saving is 0 yen with 0 cycles: the battery does not run.
- Change `capacities_kwh` of `capacities` and see where it levels off.

### Fluctuations in chemical reactions (`gillespie`)

**Try**
- Run it as it is. The trajectories fluctuate, but the mean of 2000 runs follows the rate equation. Compare stationary mean, variance and Fano factor with theory. In the reversible binding model, compare the bound fraction with the rate equation as volume changes.
- Set `volume` of `settings` to 0.1 and run. It becomes a Poisson distribution with mean 2, and trajectories drop to 0 too. The Fano factor stays 1.
- Increase `shown_runs` of `trajectories` and rerun just that Card.
- Change `on_rate` or `concentration` of `binding` and see how far it departs from the rate equation.
- Make `limit_sigma` of `check` smaller and see how far statistical fluctuation takes it off.

### An inverse problem in heat conduction (`heat-inverse`)

**Try**
- Run it as it is. Compare the naive inverse with Tikhonov regularisation. In `horizon`, inspect recoverable modes and reconstruction error as measurement time changes.
- Set `final_time` of `settings` to 0.02 and run. With the same noise, the recovery's error grows.
- Change `lambda` of `reconstruct` and rerun just that Card. Back at 0, it uses the discrepancy principle's value.
- Set `noise` of `settings` to 0 and `final_time` to 0.00005, and the recovery returns to the truth.
- Change `times` of `horizon` and find the time at which the jaggedness disappears.

### Comparing ecological models (`ecology-models`)

**Try**
- Run it as it is. Compare fit error, AICc, parameter estimates and held-out forecasts. Use the sticky note's cautions when interpreting a good score: it does not establish a mechanism, and pelts are not population counts.
- Change `fit_until` of `forecast` between 1908 and 1918 and rerun just that Card. The more years held out, the harder the forecast.
- Lower or raise `restarts` of `fit` and look at "starting points that reached the same solution" and the errors in `scores`.
- Increase `years` of `phase` and see where the logistic spiral heads.

## Draft versions (for the Agent to implement)

`casa-draft`, `gw150914-draft`, `protein-6lu7-draft`, `exoplanet-transit-draft` and `seismic-trench-draft` are projects that hold the same plans as the demos above, as Drafts only (`casa-draft` is the plan up to the RGB image, without CIAO and `spectrum`).
Each has a start Card and three Drafts, and each Draft's description says what it receives, what it does and what it passes on (almost the same text as the descriptions of the implemented demos' Cards).

Press "Generate Cards" at the top right of the Canvas and the Agent makes Cards from the Drafts, runs them and checks ([Generating Cards from Drafts](Usage.md#working-with-the-agent)).

## Dynamical systems gallery (`dynamics-draft`)

A demo in which the Agent splits six independent Drafts among helpers and implements them at the same time.

`settings` only hands the image size (`size` 480) and the random seed (`seed` 7) to the six Drafts; the Drafts are not connected to each other.
Each Draft's description gives the steps of the computation and `assert`s that check the result is right (a glider in the Game of Life moving one cell diagonally in four generations, the double pendulum's energy error being small, and so on).

Press "Generate Cards" and the Agent splits the six Drafts among helpers, and a thrush of a different colour for each helper writes and checks its own Card at the same time ([Splitting the work](features.md#splitting-the-work-delegate)).

## MNIST · comparing classifiers (Draft) (`mnist-classifiers-draft`)

A demo that has the Agent implement the same problem as [MNIST · comparing classifiers](#mnist--comparing-classifiers-mnist-classifiers). Unlike the dynamical systems gallery, the helpers' work merges into one comparison at the end.

- **Why they can merge:** each Draft's description writes out the type of the output `result` (`mnist.Result`). This shared agreement lets helpers implement independently and lets `compare` read their outputs with one type.
- **What is checked:** each Draft's description has the steps of the computation and `assert`s. Besides the counts, the 0–9 range and a minimum accuracy, they check properties that each method's result must have (kNN agreeing with brute force in numpy, logistic regression's predictions agreeing with the weighted sums, the forest's predictions agreeing with the average of its trees, formulas for the networks' parameter counts and so on). `compare` checks that the digests agree and that the accuracies agree with values recomputed from the predictions.
- **Why the counts are small:** 5000 training and 1000 test images keep verification runs short, compared with the finished version's 10000 and 2000. A project can run only one run at a time; coordinate through the helper instructions so the parent runs after the helpers finish writing.
- The Agent can find both MNIST demos with `demo_list` and read their bundled files with `demo_read`.

## csharp-calibration

The **Cross-language calibration (C#)** demo passes three C++ readings (1.25, 2.5, 3.75) through a C# Card with `factor = 1.6`, then summarizes the typed result in Python and retains a CSV. The expected calibrated values are 2, 4 and 6: sum 12 and mean 4. Change `calibrate.factor` to 2 and rerun; the sum becomes 15 and mean 5. The C# log uses a fixed-hash Newtonsoft.Json dependency. Each Card's source and the saved CSV remain available to inspect. See [C# Cards](CSharp.md) for the SDK and worker lifetime.

## Visualization → report (`visualization`)

A typed aggregate fans out to line, scatter and histogram Cards, then a Python report collects their saved PNG/SVG/PDF figures and plotted values. Change `aggregate.offset` to produce different figures with the same data size. The [Visualization guide](Visualization.md) describes the three library templates and their parameters.

## marimo notebook → typed result (`marimo`)

The upstream Card scales a measurement of 7 by its `factor` of 2. A marimo notebook visualizes the original and scaled values, and a Bash Card logs and displays the selected result of 14. The notebook invocation retains static HTML, a figure, source cells and binding and result metadata. Set the upstream `factor` to 3 and rerun it to save a result of 21. See [marimo Cards](Marimo.md).
