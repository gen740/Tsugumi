suppressPackageStartupMessages(library(metafor))

# dat.bcg: 13 trials of the BCG vaccine against tuberculosis (Colditz et al. 1994).
main <- function(ctx) {
  d <- escalc(measure = "RR", ai = tpos, bi = tneg, ci = cpos, di = cneg, data = dat.bcg)
  d <- as.data.frame(d)
  d$yi <- as.numeric(d$yi)
  d$vi <- as.numeric(d$vi)
  file <- file.path(ctx$output_directory, "bcg.csv")
  write.csv(d, file, row.names = FALSE)
  ctx$output("trials", list(value = file))
  ctx$display_table(data.frame(
    試験 = d$trial, 著者 = d$author, 年 = d$year,
    接種群 = sprintf("%d / %s", d$tpos, format(d$tpos + d$tneg, trim = TRUE)),
    対照群 = sprintf("%d / %s", d$cpos, format(d$cpos + d$cneg, trim = TRUE)),
    緯度 = d$ablat, RR = sprintf("%.2f", exp(d$yi)), `log RR` = sprintf("%.3f", d$yi), 分散 = sprintf("%.4f", d$vi),
    check.names = FALSE))
}
