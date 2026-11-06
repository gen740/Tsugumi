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
main <- function(ctx) {
  fdr <- ctx$params$fdr
  x <- read_counts(ctx$path("counts"))
  with_cell <- read.csv(ctx$path("results"))
  dds <- DESeq(DESeqDataSetFromMatrix(x$counts, x$samples, ~ dex), quiet = TRUE)
  res <- results(dds, contrast = c("dex", "trt", "untrt"), alpha = fdr)
  d <- data.frame(with_cell[, c("gene_id", "symbol")], lfc_paired = with_cell$log2fc, stat_paired = with_cell$stat, fdr_paired = with_cell$fdr,
                  lfc_unpaired = res$log2FoldChange, stat_unpaired = res$stat, fdr_unpaired = res$padj)
  file <- file.path(ctx$output_directory, "design.csv")
  write.csv(d, file, row.names = FALSE)
  ctx$output("design", list(value = file))

  both <- d[!is.na(d$stat_paired) & !is.na(d$stat_unpaired), ]
  paired <- sum(d$fdr_paired < fdr, na.rm = TRUE)
  unpaired <- sum(d$fdr_unpaired < fdr, na.rm = TRUE)
  shared <- sum(d$fdr_paired < fdr & d$fdr_unpaired < fdr, na.rm = TRUE)
  message(sprintf("FDR < %g: paired %d, unpaired %d, both %d", fdr, paired, unpaired, shared))
  long <- rbind(
    data.frame(panel = "log2 倍率変化 (推定値)", x = both$lfc_unpaired, y = both$lfc_paired),
    data.frame(panel = "Wald 統計量 (推定値 / 標準誤差)", x = both$stat_unpaired, y = both$stat_paired))
  long$panel <- factor(long$panel, unique(long$panel))
  figure <- ggplot(long, aes(x, y)) +
    geom_abline(colour = "#D55E00") +
    geom_point(size = 0.35, alpha = 0.35, colour = "#0072B2") +
    facet_wrap(~ panel, scales = "free") +
    labs(x = "~ dex (細胞株を入れない)", y = "~ cell + dex (対応あり)",
         title = sprintf("FDR < %g: 対応あり %d, 対応なし %d, 共通 %d", fdr, paired, unpaired, shared),
         subtitle = sprintf("log2FC の r = %.2f。推定値はほぼ同じで、細胞株の差を説明に回すと不確かさが小さくなる", cor(both$lfc_paired, both$lfc_unpaired))) +
    theme_minimal(base_size = 11)
  ctx$plot(print(figure), alt = sprintf("Paired %d against unpaired %d genes", paired, unpaired), width = 8.5, height = 4.4)
}
