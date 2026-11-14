# The real measurements of Gorman et al. 2014, as packaged by palmerpenguins.
main <- function(inputs, report, run) {
  outputs <- list()
  p <- palmerpenguins::penguins
  p <- p[complete.cases(p[, c("bill_length_mm", "bill_depth_mm")]), ]
  file <- file.path(run$output_directory, "penguins.csv")
  write.csv(p[, c("species", "sex", "island", "bill_length_mm", "bill_depth_mm")], file, row.names = FALSE)
  outputs[["penguins"]] <- list(value = file)
  counts <- as.data.frame.matrix(table(p$species, p$island))
  counts <- cbind(種 = rownames(counts), counts, 合計 = rowSums(counts))
  report$log(paste0(nrow(p), " penguins with both bill measurements"))
  report$table(counts)

  outputs
}
