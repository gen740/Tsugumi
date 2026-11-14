suppressPackageStartupMessages(library(SummarizedExperiment))

# The airway data: Himes et al. 2014, four airway smooth muscle cell lines, with and without dexamethasone.
main <- function(inputs, report, run) {
  outputs <- list()
  data("airway", package = "airway", envir = environment())
  counts <- assay(airway)
  genes <- rowData(airway)
  samples <- as.data.frame(colData(airway))
  table <- data.frame(gene_id = rownames(counts), symbol = genes$gene_name, counts, check.names = FALSE)
  out <- file.path(run$output_directory, "airway")
  dir.create(out)
  write.csv(table, file.path(out, "counts.csv"), row.names = FALSE)
  shown <- data.frame(sample = colnames(counts), cell = samples$cell, dex = samples$dex)
  write.csv(shown, file.path(out, "samples.csv"), row.names = FALSE)
  outputs[["airway"]] <- list(value = out)
  shown$`読まれた数 (百万)` <- sprintf("%.1f", colSums(counts) / 1e6)
  shown$`検出遺伝子` <- colSums(counts > 0)
  report$log(paste0(nrow(counts), " genes, airway ", packageVersion("airway")))
  report$table(shown)

  outputs
}
