library(ggplot2)

# Why the totals differ: rates by department, drawn by where people applied.
main <- function(ctx) {
  d <- read.csv(ctx$path("admissions"))
  wide <- aggregate(count ~ gender + dept, d, sum)
  names(wide)[3] <- "applied"
  admitted <- d[d$admit == "Admitted", c("gender", "dept", "count")]
  wide <- merge(wide, admitted)
  wide$rate <- 100 * wide$count / wide$applied
  overall <- aggregate(cbind(count, applied) ~ gender, wide, sum)
  overall$rate <- 100 * overall$count / overall$applied
  wide$gender <- factor(wide$gender, c("Male", "Female"), c("男性", "女性"))
  overall$gender <- factor(overall$gender, c("Male", "Female"), c("男性", "女性"))
  colours <- c("男性" = "#0072B2", "女性" = "#D55E00")
  figure <- ggplot(wide, aes(dept, rate, colour = gender)) +
    geom_hline(data = overall, aes(yintercept = rate, colour = gender), linetype = "dashed") +
    geom_text(data = overall, aes(x = 6.45, y = rate, label = sprintf("全体 %s %.1f%%", gender, rate)),
              hjust = 1, vjust = -0.5, size = 4, show.legend = FALSE) +
    geom_point(aes(size = applied), alpha = 0.75, position = position_dodge(width = 0.35)) +
    scale_x_discrete() +
    scale_colour_manual(values = colours, name = NULL) +
    scale_size_area(max_size = 13, name = "応募者数") +
    labs(x = "学科 (匿名化された A〜F)", y = "合格率 (%)", title = "女性の応募は合格率の低い学科に偏っていた") +
    theme_minimal(base_size = 13) +
    theme(legend.position = "top")
  ctx$plot(print(figure), alt = "Admission rate by department and gender, with the number of applicants", width = 7.5, height = 5.2)
}
