library(ggplot2)
library(ggrepel)

# Volcano plot at the thresholds in the params, with the known responders named.
main <- function(inputs, report) {
  fdr <- inputs$params$fdr
  lfc <- inputs$params$lfc
  d <- read.csv(inputs$path("results"))
  d <- d[!is.na(d$fdr) & !is.na(d$log2fc), ]
  d$class <- ifelse(d$fdr < fdr & d$log2fc >= lfc, "up", ifelse(d$fdr < fdr & d$log2fc <= -lfc, "down", "other"))
  d$y <- pmin(-log10(pmax(d$fdr, 1e-300)), 150)
  named <- d[d$symbol %in% inputs$params$known, ]
  up <- sum(d$class == "up")
  down <- sum(d$class == "down")
  report$log(paste(capture.output(print(c(fdr = fdr, lfc = lfc, up = up, down = down))), collapse = "\n"))
  figure <- ggplot(d, aes(log2fc, y, colour = class)) +
    geom_point(size = 0.5, alpha = 0.55) +
    geom_vline(xintercept = c(-lfc, lfc), linetype = "dashed", colour = "grey50") +
    geom_hline(yintercept = -log10(fdr), linetype = "dashed", colour = "grey50") +
    geom_text_repel(data = named, aes(label = symbol), colour = "black", size = 3.3, max.overlaps = Inf, seed = 1, min.segment.length = 0) +
    scale_colour_manual(values = c("up" = "#D55E00", "down" = "#0072B2", "other" = "grey75"), name = NULL) +
    guides(colour = guide_legend(override.aes = list(size = 2.5, alpha = 1))) +
    coord_cartesian(xlim = c(-8, 8)) +
    labs(x = "log2 fold change (dexamethasone / untreated)", y = "-log10 FDR (cut at 150)",
         title = sprintf("FDR < %g and |log2FC| ≥ %g: up %d, down %d", fdr, lfc, up, down)) +
    theme_minimal(base_size = 12) +
    theme(legend.position = "top")
  report$figure(print(figure), alt = sprintf("Volcano plot: %d up, %d down", up, down), width = 7.5, height = 5.2)

  invisible(NULL)
}
