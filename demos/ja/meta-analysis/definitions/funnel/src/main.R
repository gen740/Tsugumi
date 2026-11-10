suppressPackageStartupMessages(library(metafor))

# Funnel plot, Egger-type regression and trim and fill.
main <- function(ctx) {
  d <- read.csv(ctx$path("trials"))
  model <- rma(yi, vi, data = d, method = ctx$params$method)
  egger <- regtest(model)
  filled <- trimfill(model)
  message(sprintf("regtest z %.3f p %.3f; trim and fill adds %d, RR %.3f", egger$zval, egger$pval, filled$k0, exp(as.numeric(coef(filled)))))
  ctx$plot({
    par(mar = c(4.4, 4.4, 3.4, 1))
    funnel(model, atransf = exp, at = log(c(0.1, 0.25, 0.5, 1, 2, 4)), xlab = "リスク比 (対数軸)", ylab = "標準誤差", pch = 19, back = "grey95", shade = "grey95")
    title(sprintf("I² = %.1f%%, τ² = %.3f\n回帰検定 p = %.2f, trim and fill で補う試験 %d 件", model$I2, model$tau2, egger$pval, filled$k0), font.main = 1, cex.main = 1)
  }, alt = sprintf("Funnel plot, regression test p %.2f", egger$pval), width = 6.4, height = 5)
}
