suppressPackageStartupMessages(library(metafor))

# dat.bcg: 13 trials of the BCG vaccine against tuberculosis (Colditz et al. 1994).
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  d <- escalc(measure = "RR", ai = tpos, bi = tneg, ci = cpos, di = cneg, data = dat.bcg)
  d <- as.data.frame(d)
  d$yi <- as.numeric(d$yi)
  d$vi <- as.numeric(d$vi)
  file <- file.path(workspace, "bcg.csv")
  write.csv(d, file, row.names = FALSE)
  outputs[["trials"]] <- artifact_upload(file)
  # TODO(open): Port the demo display.

  outputs
}
