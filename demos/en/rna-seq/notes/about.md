## RNA-seq: genes moved by dexamethasone

The data is Bioconductor's **airway**: real data from four airway smooth muscle cell lines treated with dexamethasone for 18 hours (Himes et al., *PLoS One* 2014;9:e99625, GEO GSE52778). There are eight samples, 4 lines × with or without treatment.

A negative binomial generalised linear model estimates the treatment effect $\beta$ for each gene.

$$K_{ij}\sim \mathrm{NB}(s_j q_{ij},\ \alpha_i),\qquad \log_2 q_{ij}=\beta_{\text{cell line}(j)}+\beta_i\,x_j$$

$s_j$ is the size factor (sequencing depth), $\alpha_i$ the size of the dispersion.

**What to change**

- `min_total` of **filter**: the cut-off for removing genes read only a few times.
- `paired` of **deseq**: whether to include the cell-line term. **design** lays both side by side. Explaining the differences between lines makes the same estimate less uncertain.
- `fdr` and `lfc` of **volcano**, and the grid of **thresholds**: the number of genes changes a lot with the thresholds, and "the number of differentially expressed genes" is not a value the data decides alone.
- **compare**: how DESeq2 and edgeR agree.

FKBP5, DUSP1, TSC22D3, KLF15, PER1 and CRISPLD2 are known to go up with glucocorticoids, and are used to check the results. This is a cell-culture experiment; it shows no effect of treatment and is not medical advice.