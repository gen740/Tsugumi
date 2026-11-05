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
main <- function(ctx) {
  paired <- ctx$params$paired
  x <- read_counts(ctx$path("counts"))
  dds <- DESeqDataSetFromMatrix(x$counts, x$samples, if (paired) ~ cell + dex else ~ dex)
  dds <- DESeq(dds, quiet = TRUE)
  res <- results(dds, contrast = c("dex", "trt", "untrt"), alpha = ctx$params$fdr)
  rows <- data.frame(x$genes, base_mean = res$baseMean, log2fc = res$log2FoldChange, lfc_se = res$lfcSE,
                     stat = res$stat, p = res$pvalue, fdr = res$padj)
  file <- file.path(ctx$output_directory, "deseq2.csv")
  write.csv(rows, file, row.names = FALSE)
  ctx$output("results", list(value = file))

  significant <- !is.na(rows$fdr) & rows$fdr < ctx$params$fdr
  message(sprintf("%s design: %d genes below FDR %.2f (%d up, %d down)", if (paired) "paired" else "unpaired",
                  sum(significant), ctx$params$fdr, sum(significant & rows$log2fc > 0), sum(significant & rows$log2fc < 0)))
  shown <- rows[rows$base_mean > 0 & !is.na(rows$log2fc), ]
  shown$class <- ifelse(!is.na(shown$fdr) & shown$fdr < ctx$params$fdr, "有意", "それ以外")
  shown <- shown[order(shown$class == "有意"), ]
  figure <- ggplot(shown, aes(base_mean, pmax(pmin(log2fc, 6), -6), colour = class)) +
    geom_hline(yintercept = 0, colour = "grey50") +
    geom_point(size = 0.35, alpha = 0.5) +
    scale_x_log10() +
    scale_colour_manual(values = c("有意" = "#D55E00", "それ以外" = "grey70"), name = NULL) +
    guides(colour = guide_legend(override.aes = list(size = 2.5, alpha = 1))) +
    labs(x = "平均カウント (対数軸)", y = "log2 倍率変化 (±6 で打ち切り)",
         title = sprintf("MA プロット: FDR < %.2f で %d 遺伝子 (上昇 %d, 低下 %d)",
                         ctx$params$fdr, sum(significant), sum(significant & rows$log2fc > 0), sum(significant & rows$log2fc < 0)),
         subtitle = paste("デザイン", if (paired) "~ cell + dex" else "~ dex")) +
    theme_minimal(base_size = 12) +
    theme(legend.position = "top")
  ctx$plot(print(figure), alt = sprintf("MA plot, %d genes below FDR %.2f", sum(significant), ctx$params$fdr), width = 7.5, height = 4.6)
}
