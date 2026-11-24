## Survival analysis: the result moves with "whom" and "what"

The data is `lung`, bundled with the **survival** package: real data on 228 patients with advanced lung cancer from the North Central Cancer Treatment Group (NCCTG) (Loprinzi et al., *J Clin Oncol* 1994;12:601–607).

Survival with censoring is estimated by Kaplan–Meier, and hazard ratios come from a Cox model.

$$\hat S(t)=\prod_{t_i\le t}\Bigl(1-\frac{d_i}{n_i}\Bigr),\qquad h(t\mid x)=h_0(t)\,e^{\beta x}$$

Change age and ECOG of **cohort**, `covariates` of **cox**, and `subsets` and `adjustments` of **specs**, and rerun just that Card. The figure of **specs** lays out how far the answer to the same question moves with the specification. Each row of a subgroup has a different number of people, and people missing an adjustment variable drop out.

**zph** checks proportional hazards. Add `ph.karno` to `covariates` to see the assumption become doubtful.

These are associations in old trial data; they show no cause and are not advice on treatment or prognosis.