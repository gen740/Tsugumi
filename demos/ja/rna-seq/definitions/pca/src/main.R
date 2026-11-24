library(ggplot2)

# Principal components of the samples on the most variable genes.
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  x <- readRDS(file.path(artifact_open(inputs$normalized), "vst.rds"))
  n <- min(params$top_genes, nrow(x$vst))
  variable <- order(apply(x$vst, 1, var), decreasing = TRUE)[seq_len(n)]
  pca <- prcomp(t(x$vst[variable, ]))
  share <- 100 * pca$sdev^2 / sum(pca$sdev^2)
  rows <- data.frame(x$samples, PC1 = pca$x[, 1], PC2 = pca$x[, 2])
  file <- file.path(workspace, "pca.csv")
  write.csv(data.frame(rows, PC1_percent = share[1], PC2_percent = share[2]), file, row.names = FALSE)
  outputs[["pca"]] <- artifact_upload(file)
  rows$処理 <- ifelse(rows$dex == "trt", "dexamethasone", "未処理")
  figure <- ggplot(rows, aes(PC1, PC2, colour = 処理, shape = cell)) +
    geom_point(size = 4) +
    scale_colour_manual(values = c("dexamethasone" = "#D55E00", "未処理" = "#0072B2")) +
    labs(x = sprintf("PC1 (%.0f%%)", share[1]), y = sprintf("PC2 (%.0f%%)", share[2]), shape = "細胞株", colour = NULL,
         title = sprintf("分散の大きい %d 遺伝子の主成分", n)) +
    theme_minimal(base_size = 12)
  # TODO(open): Port the demo display.

  outputs
}
