# Who is included: an age range and a limit on the ECOG performance score.
main <- function(ctx) {
  p <- ctx$params
  d <- read.csv(ctx$path("lung"))
  keep <- d$age >= p$age_min & d$age <= p$age_max
  if (p$ecog_max < 3) keep <- keep & !is.na(d$ph.ecog) & d$ph.ecog <= p$ecog_max
  chosen <- d[keep, ]
  if (sum(chosen$event) < 5) stop("too few deaths in this cohort: ", sum(chosen$event))
  file <- file.path(ctx$output_directory, "cohort.csv")
  write.csv(chosen, file, row.names = FALSE)
  ctx$output("cohort", list(value = file))
  ctx$display_table(data.frame(
    対象 = c("全 228 人", sprintf("%d〜%d 歳, ECOG %d 以下", p$age_min, p$age_max, p$ecog_max)),
    人数 = c(nrow(d), nrow(chosen)), 死亡 = c(sum(d$event), sum(chosen$event)), 女性 = c(sum(d$female), sum(chosen$female))))
}
