library(ggplot2)

# r per stratum for all three stratifying variables, side by side.
main <- function(inputs, report) {
  d <- read.csv(inputs$path("penguins"))
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
  rows$side <- ifelse(sign(rows$r) != sign(overall), "全体と逆向き", "全体と同じ向き")
  rows$label <- sprintf("%s (n=%d)", rows$stratum, rows$n)
  verdict <- tapply(rows$side == "全体と逆向き", rows$by, sum)
  report$log(paste0("strata reversed: ", paste(names(verdict), verdict, collapse = ", ")))
  figure <- ggplot(rows, aes(r, reorder(label, r), colour = side)) +
    geom_vline(xintercept = 0, colour = "grey60") +
    geom_vline(xintercept = overall, linetype = "dashed") +
    geom_errorbar(aes(xmin = lower, xmax = upper), width = 0, orientation = "y", linewidth = 0.8) +
    geom_point(size = 2.8) +
    facet_grid(by ~ ., scales = "free_y", space = "free_y") +
    scale_colour_manual(values = c("全体と逆向き" = "#D55E00", "全体と同じ向き" = "#0072B2"), name = NULL) +
    labs(x = "Pearson の r (横線は 95% 信頼区間)", y = NULL,
         title = sprintf("層別の変数で逆転は変わる: 破線が全体 r = %.2f", overall)) +
    theme_minimal(base_size = 13) +
    theme(legend.position = "top")
  report$figure(print(figure), alt = "Correlation in each stratum for species, sex and island", width = 7.5, height = 5.2)

  invisible(NULL)
}
