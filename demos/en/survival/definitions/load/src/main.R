# survival::lung: the NCCTG advanced lung cancer trial (Loprinzi et al. 1994).
main <- function(ctx) {
  d <- survival::lung
  d$event <- as.integer(d$status == 2)  # status: 1 censored, 2 dead
  d$female <- as.integer(d$sex == 2)    # sex: 1 male, 2 female
  file <- file.path(ctx$output_directory, "lung.csv")
  write.csv(d, file, row.names = FALSE)
  ctx$output("lung", list(value = file))

  meaning <- c(time = "Days to death or censoring", event = "death 1 / censored 0", age = "Age", female = "woman 1 / man 0",
               ph.ecog = "ECOG performance status (physician's rating, 0–3)", ph.karno = "Karnofsky score (physician's rating)",
               pat.karno = "Karnofsky score (patient's rating)", meal.cal = "Calories per meal", wt.loss = "Weight loss in the last 6 months (pounds)")
  rows <- lapply(names(meaning), function(v) data.frame(
    Variable = v, Meaning = meaning[[v]], Median = format(median(d[[v]], na.rm = TRUE)),
    Range = sprintf("%g–%g", min(d[[v]], na.rm = TRUE), max(d[[v]], na.rm = TRUE)), Missing = as.character(sum(is.na(d[[v]])))))
  message(nrow(d), " patients, ", sum(d$event), " deaths, survival ", packageVersion("survival"))
  ctx$display_table(do.call(rbind, rows))
}
