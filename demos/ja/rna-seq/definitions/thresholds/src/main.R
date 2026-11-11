# How the number of genes depends on the two thresholds.
main <- function(ctx) {
  d <- read.csv(ctx$path("results"))
  d <- d[!is.na(d$fdr), ]
  lfcs <- unlist(ctx$params$lfcs)
  rows <- lapply(unlist(ctx$params$fdrs), function(f) {
    counts <- vapply(lfcs, function(l) sum(d$fdr < f & abs(d$log2fc) >= l), numeric(1))
    cells <- as.list(as.character(counts))
    names(cells) <- sprintf("|log2FC| ≥ %g", lfcs)
    data.frame(FDR = sprintf("< %g", f), cells, check.names = FALSE)
  })
  ctx$display_table(do.call(rbind, rows))
}
