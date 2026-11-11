library(ggplot2)

# r per stratum for all three stratifying variables, side by side.
main <- function(ctx) {
  d <- read.csv(ctx$path("penguins"))
  overall <- cor(d$bill_length_mm, d$bill_depth_mm)
  rows <- lapply(c("species", "sex", "island"), function(by) {
    x <- d[!is.na(d[[by]]), ]
    do.call(rbind, lapply(split(x, x[[by]]), function(s) {
      test <- cor.test(s$bill_length_mm, s$bill_depth_mm)
      data.frame(by = by, stratum = s[[by]][1], n = nrow(s), r = unname(test$estimate),
                 lower = test$conf.int[1], upper = test$conf.int[2])
    }))
  })
  rows <- do.call(rbind, rows)
  rows$by <- factor(rows$by, levels = c("species", "sex", "island"))
  rows$side <- ifelse(sign(rows$r) != sign(overall), "opposite to all", "same direction as all")
  rows$label <- sprintf("%s (n=%d)", rows$stratum, rows$n)
  verdict <- tapply(rows$side == "opposite to all", rows$by, sum)
  message("strata reversed: ", paste(names(verdict), verdict, collapse = ", "))
  figure <- ggplot(rows, aes(r, reorder(label, r), colour = side)) +
    geom_vline(xintercept = 0, colour = "grey60") +
    geom_vline(xintercept = overall, linetype = "dashed") +
    geom_errorbar(aes(xmin = lower, xmax = upper), width = 0, orientation = "y", linewidth = 0.8) +
    geom_point(size = 2.8) +
    facet_grid(by ~ ., scales = "free_y", space = "free_y") +
    scale_colour_manual(values = c("opposite to all" = "#D55E00", "same direction as all" = "#0072B2"), name = NULL) +
    labs(x = "Pearson's r (lines are 95% confidence intervals)", y = NULL,
         title = sprintf("Whether it reverses depends on the stratifying variable: the dashed line is all, r = %.2f", overall)) +
    theme_minimal(base_size = 13) +
    theme(legend.position = "top")
  ctx$plot(print(figure), alt = "Correlation in each stratum for species, sex and island", width = 7.5, height = 5.2)
}
