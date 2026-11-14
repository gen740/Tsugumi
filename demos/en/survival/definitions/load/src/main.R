# survival::lung: the NCCTG advanced lung cancer trial (Loprinzi et al. 1994).
main <- function(inputs, report, run) {
  outputs <- list()
  d <- survival::lung
  d$event <- as.integer(d$status == 2)  # status: 1 censored, 2 dead
  d$female <- as.integer(d$sex == 2)    # sex: 1 male, 2 female
  file <- file.path(run$output_directory, "lung.csv")
  write.csv(d, file, row.names = FALSE)
  outputs[["lung"]] <- list(value = file)

  meaning <- c(time = "Days to death or censoring", event = "death 1 / censored 0", age = "Age", female = "woman 1 / man 0",
               ph.ecog = "ECOG performance status (physician's rating, 0–3)", ph.karno = "Karnofsky score (physician's rating)",
               pat.karno = "Karnofsky score (patient's rating)", meal.cal = "Calories per meal", wt.loss = "Weight loss in the last 6 months (pounds)")
  rows <- lapply(names(meaning), function(v) data.frame(
    Variable = v, Meaning = meaning[[v]], Median = format(median(d[[v]], na.rm = TRUE)),
    Range = sprintf("%g–%g", min(d[[v]], na.rm = TRUE), max(d[[v]], na.rm = TRUE)), Missing = as.character(sum(is.na(d[[v]])))))
  report$log(paste0(nrow(d), " patients, ", sum(d$event), " deaths, survival ", packageVersion("survival")))
  report$table(do.call(rbind, rows))

  outputs
}
