# Pearson's r of bill length and bill depth: overall and within each stratum.
main <- function(ctx) {
  by <- ctx$params$stratify
  d <- read.csv(ctx$path("penguins"))
  if (!by %in% c("species", "sex", "island")) stop("stratify must be species, sex or island, not ", by)
  d$stratum <- d[[by]]
  d <- d[!is.na(d$stratum), ]  # sex is unknown for some penguins

  line <- function(name, x) {
    test <- cor.test(x$bill_length_mm, x$bill_depth_mm)
    data.frame(stratify = by, stratum = name, n = nrow(x), r = unname(test$estimate),
               lower = test$conf.int[1], upper = test$conf.int[2],
               slope = unname(coef(lm(bill_depth_mm ~ bill_length_mm, x))[2]))
  }
  overall <- line("All", d)
  strata <- do.call(rbind, lapply(split(d, d$stratum), function(x) line(x$stratum[1], x)))
  result <- rbind(overall, strata)
  result$reversed <- sign(result$r) != sign(overall$r)

  points <- file.path(ctx$output_directory, "points.csv")
  write.csv(d[, c("stratum", "bill_length_mm", "bill_depth_mm")], points, row.names = FALSE)
  correlations <- file.path(ctx$output_directory, "correlations.csv")
  write.csv(result, correlations, row.names = FALSE)
  ctx$output("points", list(value = points))
  ctx$output("correlations", list(value = correlations))

  shown <- data.frame(
    Stratum = result$stratum, n = result$n, r = sprintf("%.3f", result$r),
    `95%CI` = sprintf("%.2f–%.2f", result$lower, result$upper),
    Slope = sprintf("%.3f", result$slope),
    Direction = ifelse(result$stratum == "All", "", ifelse(result$reversed, "opposite to all", "same as all")),
    check.names = FALSE)
  ctx$display_table(shown)
}
