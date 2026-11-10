suppressPackageStartupMessages(library(SummarizedExperiment))

# The airway data: Himes et al. 2014, four airway smooth muscle cell lines, with and without dexamethasone.
main <- function(ctx) {
  data("airway", package = "airway", envir = environment())
  counts <- assay(airway)
  genes <- rowData(airway)
  samples <- as.data.frame(colData(airway))
  table <- data.frame(gene_id = rownames(counts), symbol = genes$gene_name, counts, check.names = FALSE)
  out <- file.path(ctx$output_directory, "airway")
  dir.create(out)
  write.csv(table, file.path(out, "counts.csv"), row.names = FALSE)
  shown <- data.frame(sample = colnames(counts), cell = samples$cell, dex = samples$dex)
  write.csv(shown, file.path(out, "samples.csv"), row.names = FALSE)
  ctx$output("airway", list(value = out))
  shown$`Reads (millions)` <- sprintf("%.1f", colSums(counts) / 1e6)
  shown$`Genes detected` <- colSums(counts > 0)
  message(nrow(counts), " genes, airway ", packageVersion("airway"))
  ctx$display_table(shown)
}
