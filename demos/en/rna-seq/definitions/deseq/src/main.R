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
main <- function(inputs, report, run) {
  outputs <- list()
  paired <- inputs$params$paired
  x <- read_counts(inputs$path("counts"))
  dds <- DESeqDataSetFromMatrix(x$counts, x$samples, if (paired) ~ cell + dex else ~ dex)
  dds <- DESeq(dds, quiet = TRUE)
  res <- results(dds, contrast = c("dex", "trt", "untrt"), alpha = inputs$params$fdr)
  rows <- data.frame(x$genes, base_mean = res$baseMean, log2fc = res$log2FoldChange, lfc_se = res$lfcSE,
                     stat = res$stat, p = res$pvalue, fdr = res$padj)
  file <- file.path(run$output_directory, "deseq2.csv")
  write.csv(rows, file, row.names = FALSE)
  outputs[["results"]] <- list(value = file)

  significant <- !is.na(rows$fdr) & rows$fdr < inputs$params$fdr
  report$log(paste0(sprintf("%s design: %d genes below FDR %.2f (%d up, %d down)", if (paired) "paired" else "unpaired",
                  sum(significant), inputs$params$fdr, sum(significant & rows$log2fc > 0), sum(significant & rows$log2fc < 0))))
  shown <- rows[rows$base_mean > 0 & !is.na(rows$log2fc), ]
  shown$class <- ifelse(!is.na(shown$fdr) & shown$fdr < inputs$params$fdr, "significant", "other")
  shown <- shown[order(shown$class == "significant"), ]
  figure <- ggplot(shown, aes(base_mean, pmax(pmin(log2fc, 6), -6), colour = class)) +
    geom_hline(yintercept = 0, colour = "grey50") +
    geom_point(size = 0.35, alpha = 0.5) +
    scale_x_log10() +
    scale_colour_manual(values = c("significant" = "#D55E00", "other" = "grey70"), name = NULL) +
    guides(colour = guide_legend(override.aes = list(size = 2.5, alpha = 1))) +
    labs(x = "Mean count (log axis)", y = "log2 fold change (cut at ±6)",
         title = sprintf("MA plot: %2$d genes at FDR < %1$.2f (up %3$d, down %4$d)",
                         inputs$params$fdr, sum(significant), sum(significant & rows$log2fc > 0), sum(significant & rows$log2fc < 0)),
         subtitle = paste("Design", if (paired) "~ cell + dex" else "~ dex")) +
    theme_minimal(base_size = 12) +
    theme(legend.position = "top")
  report$figure(print(figure), alt = sprintf("MA plot, %d genes below FDR %.2f", sum(significant), inputs$params$fdr), width = 7.5, height = 4.6)

  outputs
}
