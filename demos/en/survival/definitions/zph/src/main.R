library(survival)

# The proportional hazards check: scaled Schoenfeld residuals against time.
main <- function(inputs) {
  fit <- readRDS(file.path(artifact_open(inputs$fit), "fit.rds"))
  zph <- cox.zph(fit, transform = params$transform)
  table <- zph$table
  message(paste(capture.output(print(table)), collapse = "\n"))
  terms <- setdiff(rownames(table), "GLOBAL")
  columns <- min(length(terms), 3)
  rows <- ceiling(length(terms) / columns)
  # TODO(open): Port the demo display.

  invisible(NULL)
}
