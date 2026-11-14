library(ggplot2)
library(ggrepel)

# Volcano plot at the thresholds in the params, with the known responders named.
main <- function(inputs, report) {
  fdr <- inputs$params$fdr
  lfc <- inputs$params$lfc
  d <- read.csv(inputs$path("results"))
  d <- d[!is.na(d$fdr) & !is.na(d$log2fc), ]
  d$class <- ifelse(d$fdr < fdr & d$log2fc >= lfc, "上昇", ifelse(d$fdr < fdr & d$log2fc <= -lfc, "低下", "それ以外"))
  d$y <- pmin(-log10(pmax(d$fdr, 1e-300)), 150)
  named <- d[d$symbol %in% inputs$params$known, ]
  up <- sum(d$class == "上昇")
  down <- sum(d$class == "低下")
  report$log(paste(capture.output(print(c(fdr = fdr, lfc = lfc, up = up, down = down))), collapse = "\n"))
  figure <- ggplot(d, aes(log2fc, y, colour = class)) +
    geom_point(size = 0.5, alpha = 0.55) +
    geom_vline(xintercept = c(-lfc, lfc), linetype = "dashed", colour = "grey50") +
    geom_hline(yintercept = -log10(fdr), linetype = "dashed", colour = "grey50") +
    geom_text_repel(data = named, aes(label = symbol), colour = "black", size = 3.3, max.overlaps = Inf, seed = 1, min.segment.length = 0) +
    scale_colour_manual(values = c("上昇" = "#D55E00", "低下" = "#0072B2", "それ以外" = "grey75"), name = NULL) +
    guides(colour = guide_legend(override.aes = list(size = 2.5, alpha = 1))) +
    coord_cartesian(xlim = c(-8, 8)) +
    labs(x = "log2 倍率変化 (dexamethasone / 未処理)", y = "-log10 FDR (150 で打ち切り)",
         title = sprintf("FDR < %g かつ |log2FC| ≥ %g: 上昇 %d, 低下 %d", fdr, lfc, up, down)) +
    theme_minimal(base_size = 12) +
    theme(legend.position = "top")
  report$figure(print(figure), alt = sprintf("Volcano plot: %d up, %d down", up, down), width = 7.5, height = 5.2)

  invisible(NULL)
}
