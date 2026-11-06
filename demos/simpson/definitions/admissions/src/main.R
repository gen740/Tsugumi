# UCBAdmissions from base R's datasets: Berkeley, autumn 1973.
main <- function(ctx) {
  t <- datasets::UCBAdmissions
  rows <- as.data.frame(t, stringsAsFactors = FALSE)
  names(rows) <- c("admit", "gender", "dept", "count")
  file <- file.path(ctx$output_directory, "admissions.csv")
  write.csv(rows, file, row.names = FALSE)
  ctx$output("admissions", list(value = file))

  or <- function(m) m["Admitted", "Male"] * m["Rejected", "Female"] / (m["Rejected", "Male"] * m["Admitted", "Female"])
  line <- function(name, m) data.frame(
    学科 = name,
    男性応募 = sum(m[, "Male"]), 男性合格率 = sprintf("%.1f%%", 100 * m["Admitted", "Male"] / sum(m[, "Male"])),
    女性応募 = sum(m[, "Female"]), 女性合格率 = sprintf("%.1f%%", 100 * m["Admitted", "Female"] / sum(m[, "Female"])),
    オッズ比 = sprintf("%.2f", or(m)))
  by_dept <- do.call(rbind, lapply(dimnames(t)$Dept, function(k) line(k, t[, , k])))
  all <- line("全体 (集計)", margin.table(t, c(1, 2)))
  mh <- mantelhaen.test(t, correct = FALSE)
  common <- data.frame(学科 = "学科で調整 (Mantel–Haenszel)", 男性応募 = "", 男性合格率 = "", 女性応募 = "", 女性合格率 = "",
                       オッズ比 = sprintf("%.2f (%.2f〜%.2f)", mh$estimate, mh$conf.int[1], mh$conf.int[2]))
  message(sprintf("Mantel-Haenszel common odds ratio %.4f, X2 %.3f, p %.3f", mh$estimate, mh$statistic, mh$p.value))
  ctx$display_table(rbind(by_dept, all, common))
}
