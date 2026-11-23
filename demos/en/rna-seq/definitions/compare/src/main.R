library(ggplot2)

# Two methods on the same data: which genes do they agree on?
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  fdr <- params$fdr
  a <- read.csv(file.path(artifact_open(inputs$deseq2), "deseq2.csv"))
  b <- read.csv(file.path(artifact_open(inputs$edger), "edger.csv"))
  d <- merge(a[, c("gene_id", "symbol", "log2fc", "fdr")], b[, c("gene_id", "log2fc", "fdr")], by = "gene_id", suffixes = c("_deseq2", "_edger"))
  d$in_deseq2 <- !is.na(d$fdr_deseq2) & d$fdr_deseq2 < fdr
  d$in_edger <- !is.na(d$fdr_edger) & d$fdr_edger < fdr
  d$class <- ifelse(d$in_deseq2 & d$in_edger, "both", ifelse(d$in_deseq2, "DESeq2 only", ifelse(d$in_edger, "edgeR only", "neither")))
  n <- table(factor(d$class, c("both", "DESeq2 only", "edgeR only", "neither")))
  file <- file.path(workspace, "agreement.csv")
  write.csv(as.data.frame(n, responseName = "genes"), file, row.names = FALSE)
  outputs[["agreement"]] <- artifact_upload(file)
  overlap <- n[["both"]] / (n[["both"]] + n[["DESeq2 only"]] + n[["edgeR only"]])
  correlation <- cor(d$log2fc_deseq2, d$log2fc_edger, use = "complete.obs")
  message(paste(capture.output(print(n)), collapse = "\n"))
  unset <- sum(d$class == "edgeR only" & is.na(d$fdr_deseq2))
  shown <- d[order(d$class != "neither"), ]
  figure <- ggplot(shown, aes(log2fc_deseq2, log2fc_edger, colour = class)) +
    geom_abline(colour = "grey50") +
    geom_point(size = 0.5, alpha = 0.6) +
    scale_colour_manual(values = c("both" = "#009E73", "DESeq2 only" = "#D55E00", "edgeR only" = "#0072B2", "neither" = "grey80"), name = NULL) +
    guides(colour = guide_legend(override.aes = list(size = 2.5, alpha = 1))) +
    coord_cartesian(xlim = c(-8, 8), ylim = c(-8, 8)) +
    labs(x = "DESeq2 log2FC", y = "edgeR log2FC",
         title = sprintf("FDR < %g: both %d, DESeq2 only %d, edgeR only %d", fdr, n[["both"]], n[["DESeq2 only"]], n[["edgeR only"]]),
         subtitle = sprintf("both / either = %.0f%%, r of log2FC = %.3f\nof edgeR only, %d are genes DESeq2 gave no FDR (independent filtering and the like)",
                            100 * overlap, correlation, unset)) +
    theme_minimal(base_size = 11) +
    theme(legend.position = "top")
  # TODO(open): Port the demo display.

  outputs
}
