suppressPackageStartupMessages({
  library(DESeq2)
})
library(ggplot2)

read_counts <- function(dir) {
  d <- read.csv(file.path(dir, "counts.csv"), check.names = FALSE)
  samples <- read.csv(file.path(dir, "samples.csv"))
  m <- as.matrix(d[, samples$sample])
  rownames(m) <- d$gene_id
  samples$dex <- relevel(factor(samples$dex), "untrt")
  samples$cell <- factor(samples$cell)
  list(genes = d[, c("gene_id", "symbol")], counts = m, samples = samples)
}

# DESeq2: the design is paired (cell line + treatment) or treatment alone.
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  paired <- params$paired
  x <- read_counts(artifact_open(inputs$counts))
  dds <- DESeqDataSetFromMatrix(x$counts, x$samples, if (paired) ~ cell + dex else ~ dex)
  dds <- DESeq(dds, quiet = TRUE)
  res <- results(dds, contrast = c("dex", "trt", "untrt"), alpha = params$fdr)
  rows <- data.frame(x$genes, base_mean = res$baseMean, log2fc = res$log2FoldChange, lfc_se = res$lfcSE,
                     stat = res$stat, p = res$pvalue, fdr = res$padj)
  file <- file.path(workspace, "deseq2.csv")
  write.csv(rows, file, row.names = FALSE)
  outputs[["results"]] <- artifact_upload(file)

  significant <- !is.na(rows$fdr) & rows$fdr < params$fdr
  message(paste0(sprintf("%s design: %d genes below FDR %.2f (%d up, %d down)", if (paired) "paired" else "unpaired",
                  sum(significant), params$fdr, sum(significant & rows$log2fc > 0), sum(significant & rows$log2fc < 0))))
  shown <- rows[rows$base_mean > 0 & !is.na(rows$log2fc), ]
  shown$class <- ifelse(!is.na(shown$fdr) & shown$fdr < params$fdr, "significant", "other")
  shown <- shown[order(shown$class == "significant"), ]
  figure <- ggplot(shown, aes(base_mean, pmax(pmin(log2fc, 6), -6), colour = class)) +
    geom_hline(yintercept = 0, colour = "grey50") +
    geom_point(size = 0.35, alpha = 0.5) +
    scale_x_log10() +
    scale_colour_manual(values = c("significant" = "#D55E00", "other" = "grey70"), name = NULL) +
    guides(colour = guide_legend(override.aes = list(size = 2.5, alpha = 1))) +
    labs(x = "Mean count (log axis)", y = "log2 fold change (cut at ±6)",
         title = sprintf("MA plot: %2$d genes at FDR < %1$.2f (up %3$d, down %4$d)",
                         params$fdr, sum(significant), sum(significant & rows$log2fc > 0), sum(significant & rows$log2fc < 0)),
         subtitle = paste("Design", if (paired) "~ cell + dex" else "~ dex")) +
    theme_minimal(base_size = 12) +
    theme(legend.position = "top")
  # TODO(open): Port the demo display.

  outputs
}
