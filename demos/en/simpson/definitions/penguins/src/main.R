# The real measurements of Gorman et al. 2014, as packaged by palmerpenguins.
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  p <- palmerpenguins::penguins
  p <- p[complete.cases(p[, c("bill_length_mm", "bill_depth_mm")]), ]
  file <- file.path(workspace, "penguins.csv")
  write.csv(p[, c("species", "sex", "island", "bill_length_mm", "bill_depth_mm")], file, row.names = FALSE)
  outputs[["penguins"]] <- artifact_upload(file)
  counts <- as.data.frame.matrix(table(p$species, p$island))
  counts <- cbind(Species = rownames(counts), counts, Total = rowSums(counts))
  message(paste0(nrow(p), " penguins with both bill measurements"))
  # TODO(open): Port the demo display.

  outputs
}
