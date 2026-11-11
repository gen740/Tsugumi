# The strongest genes, and where the known responders of the literature stand.
main <- function(ctx) {
  d <- read.csv(ctx$path("results"))
  d <- d[!is.na(d$fdr), ]
  d <- d[order(d$fdr, d$p), ]
  d$rank <- seq_len(nrow(d))
  known <- ctx$params$known
  shown <- d[d$rank <= ctx$params$top | d$symbol %in% known, ]
  ctx$display_table(data.frame(
    順位 = shown$rank, 遺伝子 = shown$symbol, `平均カウント` = sprintf("%.0f", shown$base_mean),
    log2FC = sprintf("%.2f", shown$log2fc), 倍率 = sprintf("%.1f", 2^shown$log2fc), FDR = sprintf("%.2g", shown$fdr),
    既知 = ifelse(shown$symbol %in% known, "○", ""), check.names = FALSE))
}
