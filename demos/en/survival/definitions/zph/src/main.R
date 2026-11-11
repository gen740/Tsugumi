library(survival)

# The proportional hazards check: scaled Schoenfeld residuals against time.
main <- function(ctx) {
  fit <- readRDS(file.path(ctx$path("fit"), "fit.rds"))
  zph <- cox.zph(fit, transform = ctx$params$transform)
  table <- zph$table
  print(table)
  terms <- setdiff(rownames(table), "GLOBAL")
  columns <- min(length(terms), 3)
  rows <- ceiling(length(terms) / columns)
  ctx$plot({
    par(mfrow = c(rows, columns), mar = c(4.2, 4.2, 3, 1), oma = c(0, 0, 2.4, 0))
    for (i in seq_along(terms)) {
      plot(zph[i], col = "#0072B2", lwd = 2, xlab = "Time (day ticks on the transformed axis)", ylab = paste0("β(t): ", terms[i]))
      abline(h = coef(fit)[terms[i]], lty = 2)
      title(sprintf("%s  p = %.3f", terms[i], table[terms[i], "p"]), font.main = 1)
    }
    mtext(sprintf("cox.zph: overall p = %.3f (the dashed line is the model's constant coefficient)", table["GLOBAL", "p"]), outer = TRUE, cex = 1.15)
  }, alt = sprintf("Scaled Schoenfeld residuals, global p %.3f", table["GLOBAL", "p"]), width = 3.2 * columns, height = 3.4 * rows + 0.5)
}
