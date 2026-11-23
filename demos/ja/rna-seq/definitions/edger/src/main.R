suppressPackageStartupMessages(library(edgeR))

read_counts <- function(dir) {
  d <- read.csv(file.path(dir, "counts.csv"), check.names = FALSE)
  samples <- read.csv(file.path(dir, "samples.csv"))
  m <- as.matrix(d[, samples$sample])
  rownames(m) <- d$gene_id
  samples$dex <- relevel(factor(samples$dex), "untrt")
  samples$cell <- factor(samples$cell)
  list(genes = d[, c("gene_id", "symbol")], counts = m, samples = samples)
}

# edgeR: a different method on the same counts, with the paired design (quasi-likelihood F test).
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  x <- read_counts(artifact_open(inputs$counts))
  y <- DGEList(x$counts)
  y <- normLibSizes(y)
  design <- model.matrix(~ cell + dex, x$samples)
  y <- estimateDisp(y, design)
  fit <- glmQLFit(y, design)
  test <- glmQLFTest(fit, coef = "dextrt")
  all <- as.data.frame(topTags(test, n = Inf, sort.by = "none"))
  rows <- data.frame(x$genes, log2fc = all$logFC, log_cpm = all$logCPM, p = all$PValue, fdr = all$FDR)
  file <- file.path(workspace, "edger.csv")
  write.csv(rows, file, row.names = FALSE)
  outputs[["results"]] <- artifact_upload(file)
  significant <- rows$fdr < params$fdr
  # TODO(open): Port the demo display.

  outputs
}
