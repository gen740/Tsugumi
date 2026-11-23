suppressPackageStartupMessages(library(metafor))

# Fixed and random effects pooling, and the forest plot with both diamonds.
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  method <- params$method
  excluded <- unlist(params$exclude)
  d <- read.csv(file.path(artifact_open(inputs$trials), "bcg.csv"))
  d <- d[!d$trial %in% excluded, ]
  fixed <- rma(yi, vi, data = d, method = "EE")
  random <- rma(yi, vi, data = d, method = method)
  rows <- rbind(
    data.frame(model = "fixed", method = "EE", k = fixed$k, estimate = as.numeric(coef(fixed)), se = fixed$se,
               rr = exp(as.numeric(coef(fixed))), lower = exp(fixed$ci.lb), upper = exp(fixed$ci.ub), tau2 = 0, I2 = fixed$I2, Q = fixed$QE),
    data.frame(model = "random", method = method, k = random$k, estimate = as.numeric(coef(random)), se = random$se,
               rr = exp(as.numeric(coef(random))), lower = exp(random$ci.lb), upper = exp(random$ci.ub), tau2 = random$tau2, I2 = random$I2, Q = random$QE))
  file <- file.path(workspace, "pooled.csv")
  write.csv(rows, file, row.names = FALSE)
  outputs[["pooled"]] <- artifact_upload(file)
  message(paste(capture.output(print(rows)), collapse = "\n"))
  labels <- sprintf("%s, %d", d$author, d$year)
  k <- nrow(d)
  # TODO(open): Port the demo display.

  outputs
}
