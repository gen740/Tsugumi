library(ggplot2)

# Why the totals differ: rates by department, drawn by where people applied.
main <- function(inputs, report) {
  d <- read.csv(inputs$path("admissions"))
  wide <- aggregate(count ~ gender + dept, d, sum)
  names(wide)[3] <- "applied"
  admitted <- d[d$admit == "Admitted", c("gender", "dept", "count")]
  wide <- merge(wide, admitted)
  wide$rate <- 100 * wide$count / wide$applied
  overall <- aggregate(cbind(count, applied) ~ gender, wide, sum)
  overall$rate <- 100 * overall$count / overall$applied
  wide$gender <- factor(wide$gender, c("Male", "Female"), c("Men", "Women"))
  overall$gender <- factor(overall$gender, c("Male", "Female"), c("Men", "Women"))
  colours <- c("Men" = "#0072B2", "Women" = "#D55E00")
  figure <- ggplot(wide, aes(dept, rate, colour = gender)) +
    geom_hline(data = overall, aes(yintercept = rate, colour = gender), linetype = "dashed") +
    geom_text(data = overall, aes(x = 6.45, y = rate, label = sprintf("All %s %.1f%%", gender, rate)),
              hjust = 1, vjust = -0.5, size = 4, show.legend = FALSE) +
    geom_point(aes(size = applied), alpha = 0.75, position = position_dodge(width = 0.35)) +
    scale_x_discrete() +
    scale_colour_manual(values = colours, name = NULL) +
    scale_size_area(max_size = 13, name = "Applicants") +
    labs(x = "Department (anonymised A–F)", y = "Admission rate (%)", title = "Women's applications leaned towards departments with low admission rates") +
    theme_minimal(base_size = 13) +
    theme(legend.position = "top")
  report$figure(print(figure), alt = "Admission rate by department and gender, with the number of applicants", width = 7.5, height = 5.2)

  invisible(NULL)
}
