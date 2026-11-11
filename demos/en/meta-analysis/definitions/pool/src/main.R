suppressPackageStartupMessages(library(metafor))

# Fixed and random effects pooling, and the forest plot with both diamonds.
main <- function(ctx) {
  method <- ctx$params$method
  excluded <- unlist(ctx$params$exclude)
  d <- read.csv(ctx$path("trials"))
  d <- d[!d$trial %in% excluded, ]
  fixed <- rma(yi, vi, data = d, method = "EE")
  random <- rma(yi, vi, data = d, method = method)
  rows <- rbind(
    data.frame(model = "fixed", method = "EE", k = fixed$k, estimate = as.numeric(coef(fixed)), se = fixed$se,
               rr = exp(as.numeric(coef(fixed))), lower = exp(fixed$ci.lb), upper = exp(fixed$ci.ub), tau2 = 0, I2 = fixed$I2, Q = fixed$QE),
    data.frame(model = "random", method = method, k = random$k, estimate = as.numeric(coef(random)), se = random$se,
               rr = exp(as.numeric(coef(random))), lower = exp(random$ci.lb), upper = exp(random$ci.ub), tau2 = random$tau2, I2 = random$I2, Q = random$QE))
  file <- file.path(ctx$output_directory, "pooled.csv")
  write.csv(rows, file, row.names = FALSE)
  ctx$output("pooled", list(value = file))
  print(rows)

  labels <- sprintf("%s, %d", d$author, d$year)
  k <- nrow(d)
  ctx$plot({
    par(mar = c(4, 0, 2.6, 0), cex = 0.95)
    forest(random, slab = labels, atransf = exp, at = log(c(0.1, 0.25, 0.5, 1, 2, 4)), xlim = c(-7.2, 4.4), alim = log(c(0.1, 4)),
           ylim = c(-2.5, k + 3), addfit = FALSE, xlab = "Risk ratio (log axis)",
           header = c("Trial", "RR [95% CI]"))
    addpoly(fixed, row = -1, mlab = "Fixed effect (EE)", atransf = exp)
    addpoly(random, row = -2, mlab = sprintf("Random effects (%s)", method), atransf = exp)
    abline(h = 0)
    title(sprintf("BCG vaccine, %d trials: I² = %.1f%%, τ² = %.3f, Q = %.1f", k, random$I2, random$tau2, random$QE), font.main = 1)
  }, alt = sprintf("Forest plot with fixed %.3f and random %.3f pooled risk ratios", rows$rr[1], rows$rr[2]), width = 8.5, height = 5.6)
}
