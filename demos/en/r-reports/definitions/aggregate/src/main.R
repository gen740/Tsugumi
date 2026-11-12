main <- function(ctx) {
  observations <- c(2, 4, 8, 10) * ctx$params$scale
  values <- c(mean(observations), min(observations), max(observations))
  ctx$output("data", list(values = values))
  ctx$display_table(data.frame(statistic = c("mean", "minimum", "maximum"), value = values))
}
