# The strongest genes, and where the known responders of the literature stand.
main <- function(ctx) {
  d <- read.csv(ctx$path("results"))
  d <- d[!is.na(d$fdr), ]
  d <- d[order(d$fdr, d$p), ]
  d$rank <- seq_len(nrow(d))
  known <- ctx$params$known
  shown <- d[d$rank <= ctx$params$top | d$symbol %in% known, ]
  ctx$display_table(data.frame(
    Rank = shown$rank, Gene = shown$symbol, `Mean count` = sprintf("%.0f", shown$base_mean),
    log2FC = sprintf("%.2f", shown$log2fc), Fold = sprintf("%.1f", 2^shown$log2fc), FDR = sprintf("%.2g", shown$fdr),
    Known = ifelse(shown$symbol %in% known, "○", ""), check.names = FALSE))
}
