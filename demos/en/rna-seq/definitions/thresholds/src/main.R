# How the number of genes depends on the two thresholds.
main <- function(inputs) {
  d <- read.csv(list.files(artifact_open(inputs$results), full.names = TRUE)[[1]])
  d <- d[!is.na(d$fdr), ]
  lfcs <- unlist(params$lfcs)
  rows <- lapply(unlist(params$fdrs), function(f) {
    counts <- vapply(lfcs, function(l) sum(d$fdr < f & abs(d$log2fc) >= l), numeric(1))
    cells <- as.list(as.character(counts))
    names(cells) <- sprintf("|log2FC| ≥ %g", lfcs)
    data.frame(FDR = sprintf("< %g", f), cells, check.names = FALSE)
  })
  # TODO(open): Port the demo display.

  invisible(NULL)
}
