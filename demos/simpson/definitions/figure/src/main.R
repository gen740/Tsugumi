library(ggplot2)

# The same points with the overall line and one line per stratum.
main <- function(ctx) {
  d <- read.csv(ctx$path("points"))
  r <- read.csv(ctx$path("correlations"))
  strata <- r[r$stratum != "全体", ]
  labels <- setNames(sprintf("%s (r = %.2f)", strata$stratum, strata$r), strata$stratum)
  figure <- ggplot(d, aes(bill_length_mm, bill_depth_mm)) +
    geom_point(aes(colour = stratum), alpha = 0.55, size = 1.4) +
    geom_smooth(aes(colour = stratum), method = "lm", formula = y ~ x, se = FALSE, linewidth = 0.9) +
    geom_smooth(method = "lm", formula = y ~ x, se = FALSE, colour = "black", linetype = "dashed", linewidth = 0.9) +
    scale_colour_manual(values = c("#D55E00", "#0072B2", "#009E73")[seq_len(nrow(strata))], labels = labels, name = NULL) +
    labs(x = "くちばしの長さ (mm)", y = "くちばしの厚さ (mm)",
         title = sprintf("%s で層別: 全体 (破線) r = %.2f", r$stratify[1], r$r[r$stratum == "全体"])) +
    theme_minimal(base_size = 13) +
    theme(legend.position = "top")
  ctx$plot(print(figure), alt = "Bill length against bill depth, overall and by stratum", width = 7.5, height = 5.2)
}
