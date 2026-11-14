# How the number of genes depends on the two thresholds.
main <- function(inputs, report) {
  d <- read.csv(inputs$path("results"))
  d <- d[!is.na(d$fdr), ]
  lfcs <- unlist(inputs$params$lfcs)
  rows <- lapply(unlist(inputs$params$fdrs), function(f) {
    counts <- vapply(lfcs, function(l) sum(d$fdr < f & abs(d$log2fc) >= l), numeric(1))
    cells <- as.list(as.character(counts))
    names(cells) <- sprintf("|log2FC| ≥ %g", lfcs)
    data.frame(FDR = sprintf("< %g", f), cells, check.names = FALSE)
  })
  report$table(do.call(rbind, rows))

  invisible(NULL)
}
