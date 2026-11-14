main <- function(inputs, report) {
  outputs <- list()
  observations <- c(2, 4, 8, 10) * inputs$params$scale
  values <- c(mean(observations), min(observations), max(observations))
  outputs[["data"]] <- list(values = values)
  report$table(data.frame(statistic = c("mean", "minimum", "maximum"), value = values))

  outputs
}
