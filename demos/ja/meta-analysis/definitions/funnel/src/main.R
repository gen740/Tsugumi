suppressPackageStartupMessages(library(metafor))

# Funnel plot, Egger-type regression and trim and fill.
main <- function(inputs) {
  d <- read.csv(file.path(artifact_open(inputs$trials), "bcg.csv"))
  model <- rma(yi, vi, data = d, method = params$method)
  egger <- regtest(model)
  filled <- trimfill(model)
  message(paste0(sprintf("regtest z %.3f p %.3f; trim and fill adds %d, RR %.3f", egger$zval, egger$pval, filled$k0, exp(as.numeric(coef(filled))))))
  # TODO(open): Port the demo display.

  invisible(NULL)
}
