# Who is included: an age range and a limit on the ECOG performance score.
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  p <- inputs$params
  d <- read.csv(file.path(artifact_open(inputs$lung), "lung.csv"))
  keep <- d$age >= p$age_min & d$age <= p$age_max
  if (p$ecog_max < 3) keep <- keep & !is.na(d$ph.ecog) & d$ph.ecog <= p$ecog_max
  chosen <- d[keep, ]
  if (sum(chosen$event) < 5) stop("too few deaths in this cohort: ", sum(chosen$event))
  file <- file.path(workspace, "cohort.csv")
  write.csv(chosen, file, row.names = FALSE)
  outputs[["cohort"]] <- artifact_upload(file)
  # TODO(open): Port the demo display.

  outputs
}
