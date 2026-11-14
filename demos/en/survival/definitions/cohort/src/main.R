# Who is included: an age range and a limit on the ECOG performance score.
main <- function(inputs, report, run) {
  outputs <- list()
  p <- inputs$params
  d <- read.csv(inputs$path("lung"))
  keep <- d$age >= p$age_min & d$age <= p$age_max
  if (p$ecog_max < 3) keep <- keep & !is.na(d$ph.ecog) & d$ph.ecog <= p$ecog_max
  chosen <- d[keep, ]
  if (sum(chosen$event) < 5) stop("too few deaths in this cohort: ", sum(chosen$event))
  file <- file.path(run$output_directory, "cohort.csv")
  write.csv(chosen, file, row.names = FALSE)
  outputs[["cohort"]] <- list(value = file)
  report$table(data.frame(
    Subjects = c("All 228", sprintf("Aged %d–%d, ECOG %d or less", p$age_min, p$age_max, p$ecog_max)),
    People = c(nrow(d), nrow(chosen)), Deaths = c(sum(d$event), sum(chosen$event)), Women = c(sum(d$female), sum(chosen$female))))

  outputs
}
