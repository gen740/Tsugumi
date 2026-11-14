library(ggplot2)

# Drops genes with hardly any reads: those carry no information on a difference.
main <- function(inputs, report, run) {
  outputs <- list()
  least <- inputs$params$min_total
  d <- read.csv(file.path(inputs$path("airway"), "counts.csv"), check.names = FALSE)
  samples <- read.csv(file.path(inputs$path("airway"), "samples.csv"))
  total <- rowSums(d[, samples$sample])
  keep <- total >= least
  out <- file.path(run$output_directory, "counts")
  dir.create(out)
  write.csv(d[keep, ], file.path(out, "counts.csv"), row.names = FALSE)
  file.copy(file.path(inputs$path("airway"), "samples.csv"), file.path(out, "samples.csv"))
  outputs[["counts"]] <- list(value = out)

  shown <- data.frame(total = log10(total[total > 0]))
  figure <- ggplot(shown, aes(total)) +
    geom_histogram(bins = 60, fill = "#0072B2", alpha = 0.8) +
    geom_vline(xintercept = log10(max(least, 1)), colour = "#D55E00", linewidth = 0.9) +
    labs(x = "全サンプルの合計リード数 (log10)", y = "遺伝子の数",
         title = sprintf("%d 遺伝子のうち %d を残す (合計 %d 以上)", nrow(d), sum(keep), least),
         subtitle = sprintf("全サンプルで 0 の %d 遺伝子は図に入らない", sum(total == 0))) +
    theme_minimal(base_size = 12)
  report$figure(print(figure), alt = sprintf("%d of %d genes kept", sum(keep), nrow(d)), width = 6.5, height = 3.6)

  outputs
}
