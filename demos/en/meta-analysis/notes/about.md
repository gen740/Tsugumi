## Meta-analysis: the BCG vaccine and tuberculosis

The data is `dat.bcg`, bundled with **metafor**: real data from 13 clinical trials of the BCG vaccine against tuberculosis (Colditz et al., *JAMA* 1994;271:698–702).

Each trial's log risk ratio $y_i$ and its variance $v_i$ are pooled with weights $w_i$.

$$\hat\mu=\frac{\sum_i w_i y_i}{\sum_i w_i},\qquad w_i=\frac{1}{v_i+\tau^2}$$

$\tau^2=0$ is the fixed effect; allowing for heterogeneity between trials, $\tau^2>0$, is random effects. $I^2$ is the share of the total variation taken by the differences between trials.

**moderator** is the known explanation, a meta-regression on the latitude of the trial sites (the further from the equator, the larger the effect). It is an observational association at the level of trials, though, and latitude itself cannot be called the cause.

Change `method` (DL, REML, ...) and `exclude` (trial numbers to leave out) of **pool** and rerun just that Card. **sensitivity** lays out the change when one trial is left out and when the estimator changes.

An example of statistical methods for old trials, not advice about vaccination.