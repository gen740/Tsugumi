# survival::lung: the NCCTG advanced lung cancer trial (Loprinzi et al. 1994).
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  d <- survival::lung
  d$event <- as.integer(d$status == 2)  # status: 1 censored, 2 dead
  d$female <- as.integer(d$sex == 2)    # sex: 1 male, 2 female
  file <- file.path(workspace, "lung.csv")
  write.csv(d, file, row.names = FALSE)
  outputs[["lung"]] <- artifact_upload(file)

  meaning <- c(time = "生存または打ち切りまでの日数", event = "死亡 1 / 打ち切り 0", age = "年齢", female = "女性 1 / 男性 0",
               ph.ecog = "ECOG 全身状態 (医師評価 0〜3)", ph.karno = "Karnofsky スコア (医師評価)",
               pat.karno = "Karnofsky スコア (患者評価)", meal.cal = "食事のカロリー", wt.loss = "直近 6 か月の体重減少 (ポンド)")
  rows <- lapply(names(meaning), function(v) data.frame(
    変数 = v, 意味 = meaning[[v]], 中央値 = format(median(d[[v]], na.rm = TRUE)),
    範囲 = sprintf("%g〜%g", min(d[[v]], na.rm = TRUE), max(d[[v]], na.rm = TRUE)), 欠損 = as.character(sum(is.na(d[[v]])))))
  message(paste0(nrow(d), " patients, ", sum(d$event), " deaths, survival ", packageVersion("survival")))
  # TODO(open): Port the demo display.

  outputs
}
