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

# The same counts, with and without the cell line in the model.
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  fdr <- params$fdr
  x <- read_counts(artifact_open(inputs$counts))
  with_cell <- read.csv(list.files(artifact_open(inputs$results), full.names = TRUE)[[1]])
  dds <- DESeq(DESeqDataSetFromMatrix(x$counts, x$samples, ~ dex), quiet = TRUE)
  res <- results(dds, contrast = c("dex", "trt", "untrt"), alpha = fdr)
  d <- data.frame(with_cell[, c("gene_id", "symbol")], lfc_paired = with_cell$log2fc, stat_paired = with_cell$stat, fdr_paired = with_cell$fdr,
                  lfc_unpaired = res$log2FoldChange, stat_unpaired = res$stat, fdr_unpaired = res$padj)
  file <- file.path(workspace, "design.csv")
  write.csv(d, file, row.names = FALSE)
  outputs[["design"]] <- artifact_upload(file)

  both <- d[!is.na(d$stat_paired) & !is.na(d$stat_unpaired), ]
  paired <- sum(d$fdr_paired < fdr, na.rm = TRUE)
  unpaired <- sum(d$fdr_unpaired < fdr, na.rm = TRUE)
  shared <- sum(d$fdr_paired < fdr & d$fdr_unpaired < fdr, na.rm = TRUE)
  message(paste0(sprintf("FDR < %g: paired %d, unpaired %d, both %d", fdr, paired, unpaired, shared)))
  long <- rbind(
    data.frame(panel = "log2 fold change (estimate)", x = both$lfc_unpaired, y = both$lfc_paired),
    data.frame(panel = "Wald statistic (estimate / standard error)", x = both$stat_unpaired, y = both$stat_paired))
  long$panel <- factor(long$panel, unique(long$panel))
  figure <- ggplot(long, aes(x, y)) +
    geom_abline(colour = "#D55E00") +
    geom_point(size = 0.35, alpha = 0.35, colour = "#0072B2") +
    facet_wrap(~ panel, scales = "free") +
    labs(x = "~ dex (without the cell line)", y = "~ cell + dex (paired)",
         title = sprintf("FDR < %g: paired %d, unpaired %d, shared %d", fdr, paired, unpaired, shared),
         subtitle = sprintf("r of log2FC = %.2f. The estimates are nearly the same; explaining the differences between cell lines makes them less uncertain", cor(both$lfc_paired, both$lfc_unpaired))) +
    theme_minimal(base_size = 11)
  # TODO(open): Port the demo display.

  outputs
}
