# The real measurements of Gorman et al. 2014, as packaged by palmerpenguins.
main <- function(ctx) {
  p <- palmerpenguins::penguins
  p <- p[complete.cases(p[, c("bill_length_mm", "bill_depth_mm")]), ]
  file <- file.path(ctx$output_directory, "penguins.csv")
  write.csv(p[, c("species", "sex", "island", "bill_length_mm", "bill_depth_mm")], file, row.names = FALSE)
  ctx$output("penguins", list(value = file))
  counts <- as.data.frame.matrix(table(p$species, p$island))
  counts <- cbind(種 = rownames(counts), counts, 合計 = rowSums(counts))
  message(nrow(p), " penguins with both bill measurements")
  ctx$display_table(counts)
}
