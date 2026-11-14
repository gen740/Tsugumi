# UCBAdmissions from base R's datasets: Berkeley, autumn 1973.
main <- function(inputs, report, run) {
  outputs <- list()
  t <- datasets::UCBAdmissions
  rows <- as.data.frame(t, stringsAsFactors = FALSE)
  names(rows) <- c("admit", "gender", "dept", "count")
  file <- file.path(run$output_directory, "admissions.csv")
  write.csv(rows, file, row.names = FALSE)
  outputs[["admissions"]] <- list(value = file)

  or <- function(m) m["Admitted", "Male"] * m["Rejected", "Female"] / (m["Rejected", "Male"] * m["Admitted", "Female"])
  line <- function(name, m) data.frame(
    Department = name,
    `Male applicants` = sum(m[, "Male"]), `Male admit rate` = sprintf("%.1f%%", 100 * m["Admitted", "Male"] / sum(m[, "Male"])),
    `Female applicants` = sum(m[, "Female"]), `Female admit rate` = sprintf("%.1f%%", 100 * m["Admitted", "Female"] / sum(m[, "Female"])),
    `Odds ratio` = sprintf("%.2f", or(m)))
  by_dept <- do.call(rbind, lapply(dimnames(t)$Dept, function(k) line(k, t[, , k])))
  all <- line("All (aggregated)", margin.table(t, c(1, 2)))
  mh <- mantelhaen.test(t, correct = FALSE)
  common <- data.frame(Department = "Adjusted for department (Mantel–Haenszel)", `Male applicants` = "", `Male admit rate` = "", `Female applicants` = "", `Female admit rate` = "",
                       `Odds ratio` = sprintf("%.2f (%.2f–%.2f)", mh$estimate, mh$conf.int[1], mh$conf.int[2]))
  report$log(paste0(sprintf("Mantel-Haenszel common odds ratio %.4f, X2 %.3f, p %.3f", mh$estimate, mh$statistic, mh$p.value)))
  report$table(rbind(by_dept, all, common))

  outputs
}
