library(ggplot2)

# The same points with the overall line and one line per stratum.
main <- function(inputs) {
  d <- read.csv(file.path(artifact_open(inputs$points), "points.csv"))
  r <- read.csv(file.path(artifact_open(inputs$correlations), "correlations.csv"))
  strata <- r[r$stratum != "All", ]
  labels <- setNames(sprintf("%s (r = %.2f)", strata$stratum, strata$r), strata$stratum)
  figure <- ggplot(d, aes(bill_length_mm, bill_depth_mm)) +
    geom_point(aes(colour = stratum), alpha = 0.55, size = 1.4) +
    geom_smooth(aes(colour = stratum), method = "lm", formula = y ~ x, se = FALSE, linewidth = 0.9) +
    geom_smooth(method = "lm", formula = y ~ x, se = FALSE, colour = "black", linetype = "dashed", linewidth = 0.9) +
    scale_colour_manual(values = c("#D55E00", "#0072B2", "#009E73")[seq_len(nrow(strata))], labels = labels, name = NULL) +
    labs(x = "Bill length (mm)", y = "Bill depth (mm)",
         title = sprintf("Stratified by %s: all (dashed) r = %.2f", r$stratify[1], r$r[r$stratum == "All"])) +
    theme_minimal(base_size = 13) +
    theme(legend.position = "top")
  # TODO(open): Port the demo display.

  invisible(NULL)
}
