library(ggplot2)

# Two methods on the same data: which genes do they agree on?
main <- function(ctx) {
  fdr <- ctx$params$fdr
  a <- read.csv(ctx$path("deseq2"))
  b <- read.csv(ctx$path("edger"))
  d <- merge(a[, c("gene_id", "symbol", "log2fc", "fdr")], b[, c("gene_id", "log2fc", "fdr")], by = "gene_id", suffixes = c("_deseq2", "_edger"))
  d$in_deseq2 <- !is.na(d$fdr_deseq2) & d$fdr_deseq2 < fdr
  d$in_edger <- !is.na(d$fdr_edger) & d$fdr_edger < fdr
  d$class <- ifelse(d$in_deseq2 & d$in_edger, "両方", ifelse(d$in_deseq2, "DESeq2 のみ", ifelse(d$in_edger, "edgeR のみ", "どちらでもない")))
  n <- table(factor(d$class, c("両方", "DESeq2 のみ", "edgeR のみ", "どちらでもない")))
  file <- file.path(ctx$output_directory, "agreement.csv")
  write.csv(as.data.frame(n, responseName = "genes"), file, row.names = FALSE)
  ctx$output("agreement", list(value = file))
  overlap <- n[["両方"]] / (n[["両方"]] + n[["DESeq2 のみ"]] + n[["edgeR のみ"]])
  correlation <- cor(d$log2fc_deseq2, d$log2fc_edger, use = "complete.obs")
  print(n)
  unset <- sum(d$class == "edgeR のみ" & is.na(d$fdr_deseq2))
  shown <- d[order(d$class != "どちらでもない"), ]
  figure <- ggplot(shown, aes(log2fc_deseq2, log2fc_edger, colour = class)) +
    geom_abline(colour = "grey50") +
    geom_point(size = 0.5, alpha = 0.6) +
    scale_colour_manual(values = c("両方" = "#009E73", "DESeq2 のみ" = "#D55E00", "edgeR のみ" = "#0072B2", "どちらでもない" = "grey80"), name = NULL) +
    guides(colour = guide_legend(override.aes = list(size = 2.5, alpha = 1))) +
    coord_cartesian(xlim = c(-8, 8), ylim = c(-8, 8)) +
    labs(x = "DESeq2 の log2FC", y = "edgeR の log2FC",
         title = sprintf("FDR < %g: 両方 %d, DESeq2 のみ %d, edgeR のみ %d", fdr, n[["両方"]], n[["DESeq2 のみ"]], n[["edgeR のみ"]]),
         subtitle = sprintf("両方 / どちらか = %.0f%%, log2FC の r = %.3f\nedgeR のみのうち %d は DESeq2 が FDR を付けなかった遺伝子 (独立フィルタなど)",
                            100 * overlap, correlation, unset)) +
    theme_minimal(base_size = 11) +
    theme(legend.position = "top")
  ctx$plot(print(figure), alt = sprintf("DESeq2 and edgeR agree on %d genes", n[["両方"]]), width = 7, height = 5)
}
