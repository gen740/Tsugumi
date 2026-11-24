# The strongest genes, and where the known responders of the literature stand.
main <- function(inputs) {
  d <- read.csv(list.files(artifact_open(inputs$results), full.names = TRUE)[[1]])
  d <- d[!is.na(d$fdr), ]
  d <- d[order(d$fdr, d$p), ]
  d$rank <- seq_len(nrow(d))
  known <- params$known
  shown <- d[d$rank <= params$top | d$symbol %in% known, ]
  # TODO(open): Port the demo display.

  invisible(NULL)
}
