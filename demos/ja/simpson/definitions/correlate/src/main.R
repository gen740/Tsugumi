# Pearson's r of bill length and bill depth: overall and within each stratum.
main <- function(inputs, report, run) {
  outputs <- list()
  by <- inputs$params$stratify
  d <- read.csv(inputs$path("penguins"))
  if (!by %in% c("species", "sex", "island")) stop("stratify must be species, sex or island, not ", by)
  d$stratum <- d[[by]]
  d <- d[!is.na(d$stratum), ]  # sex is unknown for some penguins

  line <- function(name, x) {
    test <- cor.test(x$bill_length_mm, x$bill_depth_mm)
    data.frame(stratify = by, stratum = name, n = nrow(x), r = unname(test$estimate),
               lower = test$conf.int[1], upper = test$conf.int[2],
               slope = unname(coef(lm(bill_depth_mm ~ bill_length_mm, x))[2]))
  }
  overall <- line("全体", d)
  strata <- do.call(rbind, lapply(split(d, d$stratum), function(x) line(x$stratum[1], x)))
  result <- rbind(overall, strata)
  result$reversed <- sign(result$r) != sign(overall$r)

  points <- file.path(run$output_directory, "points.csv")
  write.csv(d[, c("stratum", "bill_length_mm", "bill_depth_mm")], points, row.names = FALSE)
  correlations <- file.path(run$output_directory, "correlations.csv")
  write.csv(result, correlations, row.names = FALSE)
  outputs[["points"]] <- list(value = points)
  outputs[["correlations"]] <- list(value = correlations)

  shown <- data.frame(
    層 = result$stratum, n = result$n, r = sprintf("%.3f", result$r),
    `95%CI` = sprintf("%.2f〜%.2f", result$lower, result$upper),
    傾き = sprintf("%.3f", result$slope),
    向き = ifelse(result$stratum == "全体", "", ifelse(result$reversed, "全体と逆", "全体と同じ")),
    check.names = FALSE)
  report$table(shown)

  outputs
}
