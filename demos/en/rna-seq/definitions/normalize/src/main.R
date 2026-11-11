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

# Size factors by the median of ratios, and the variance-stabilized matrix for the PCA.
main <- function(ctx) {
  x <- read_counts(ctx$path("counts"))
  dds <- DESeqDataSetFromMatrix(x$counts, x$samples, ~ dex)
  dds <- estimateSizeFactors(dds)
  factors <- sizeFactors(dds)
  vst <- assay(varianceStabilizingTransformation(dds, blind = TRUE))
  out <- file.path(ctx$output_directory, "normalized")
  dir.create(out)
  write.csv(data.frame(x$samples, size_factor = factors), file.path(out, "size_factors.csv"), row.names = FALSE)
  saveRDS(list(vst = vst, samples = x$samples), file.path(out, "vst.rds"))
  ctx$output("normalized", list(value = out))

  long <- function(m, what) data.frame(sample = rep(colnames(m), each = nrow(m)), value = log2(as.vector(m) + 1), what = what)
  before <- x$counts[rowSums(x$counts == 0) == 0, ]
  after <- sweep(before, 2, factors, "/")
  shown <- rbind(long(before, "Raw counts"), long(after, "Divided by size factors"))
  shown$what <- factor(shown$what, c("Raw counts", "Divided by size factors"))
  labels <- setNames(sprintf("%s\n%.2f", x$samples$sample, factors), x$samples$sample)
  figure <- ggplot(shown, aes(sample, value)) +
    geom_boxplot(outlier.shape = NA, fill = "#56B4E9", width = 0.6) +
    facet_wrap(~ what) +
    coord_cartesian(ylim = c(4, 14)) +
    scale_x_discrete(labels = labels) +
    labs(x = NULL, y = "log2(count + 1)", title = "Evening out sequencing depth with size factors from the median of ratios",
         subtitle = "The numbers under the sample names are the size factors. The distribution of genes read in every sample") +
    theme_minimal(base_size = 11) +
    theme(axis.text.x = element_text(angle = 90, vjust = 0.5, hjust = 1, size = 7))
  ctx$plot(print(figure), alt = "Log counts by sample before and after normalization", width = 8, height = 4.2)
}
