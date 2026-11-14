library(ggplot2)

# Principal components of the samples on the most variable genes.
main <- function(inputs, report, run) {
  outputs <- list()
  x <- readRDS(file.path(inputs$path("normalized"), "vst.rds"))
  n <- min(inputs$params$top_genes, nrow(x$vst))
  variable <- order(apply(x$vst, 1, var), decreasing = TRUE)[seq_len(n)]
  pca <- prcomp(t(x$vst[variable, ]))
  share <- 100 * pca$sdev^2 / sum(pca$sdev^2)
  rows <- data.frame(x$samples, PC1 = pca$x[, 1], PC2 = pca$x[, 2])
  file <- file.path(run$output_directory, "pca.csv")
  write.csv(data.frame(rows, PC1_percent = share[1], PC2_percent = share[2]), file, row.names = FALSE)
  outputs[["pca"]] <- list(value = file)
  rows$treatment <- ifelse(rows$dex == "trt", "dexamethasone", "untreated")
  figure <- ggplot(rows, aes(PC1, PC2, colour = treatment, shape = cell)) +
    geom_point(size = 4) +
    scale_colour_manual(values = c("dexamethasone" = "#D55E00", "untreated" = "#0072B2")) +
    labs(x = sprintf("PC1 (%.0f%%)", share[1]), y = sprintf("PC2 (%.0f%%)", share[2]), shape = "Cell line", colour = NULL,
         title = sprintf("Principal components of the %d most variable genes", n)) +
    theme_minimal(base_size = 12)
  report$figure(print(figure), alt = sprintf("PCA: PC1 %.0f%%, PC2 %.0f%%", share[1], share[2]), width = 6.5, height = 4.4)

  outputs
}
