library(survival)
library(ggplot2)

# Kaplan-Meier curves by a grouping variable, with the log-rank test.
main <- function(ctx) {
  by <- ctx$params$group
  d <- read.csv(ctx$path("cohort"))
  d$group <- switch(by,
    sex = ifelse(d$female == 1, "Women", "Men"),
    ecog = ifelse(is.na(d$ph.ecog), NA, paste0("ECOG ", pmin(d$ph.ecog, 2), ifelse(d$ph.ecog >= 2, "+", ""))),
    age = ifelse(d$age >= 65, "65 and over", "under 65"),
    stop("group must be sex, ecog or age, not ", by))
  d <- d[!is.na(d$group), ]
  fit <- survfit(Surv(time, event) ~ group, d)
  test <- survdiff(Surv(time, event) ~ group, d)
  p <- pchisq(test$chisq, length(test$n) - 1, lower.tail = FALSE)

  s <- summary(fit, censored = TRUE)
  curves <- data.frame(group = sub("group=", "", s$strata), time = s$time, at_risk = s$n.risk, deaths = s$n.event,
                       censored = s$n.censor, survival = s$surv, lower = s$lower, upper = s$upper)
  table <- summary(fit)$table
  medians <- data.frame(group = sub("group=", "", rownames(table)), n = table[, "records"], deaths = table[, "events"],
                        median = table[, "median"], lower = table[, "0.95LCL"], upper = table[, "0.95UCL"],
                        logrank_chisq = test$chisq, logrank_p = p)
  out <- file.path(ctx$output_directory, "km")
  dir.create(out)
  write.csv(curves, file.path(out, "curves.csv"), row.names = FALSE)
  write.csv(medians, file.path(out, "medians.csv"), row.names = FALSE)
  ctx$output("km", list(value = out))

  labels <- setNames(sprintf("%s  n=%d  median %s days", medians$group, medians$n,
                             ifelse(is.na(medians$median), "not reached", sprintf("%.0f", medians$median))), medians$group)
  start <- data.frame(group = medians$group, time = 0, survival = 1, lower = 1, upper = 1)
  line <- rbind(start, curves[, names(start)])
  figure <- ggplot(line, aes(time, survival, colour = group, fill = group)) +
    geom_ribbon(aes(ymin = lower, ymax = upper), alpha = 0.12, colour = NA, na.rm = TRUE) +
    geom_step(linewidth = 0.8) +
    geom_point(data = curves[curves$censored > 0, ], shape = 3, size = 1.4, show.legend = FALSE) +
    scale_colour_manual(values = c("#0072B2", "#D55E00", "#009E73", "#CC79A7"), labels = labels, name = NULL, aesthetics = c("colour", "fill")) +
    scale_y_continuous(limits = c(0, 1)) +
    labs(x = "Days", y = "Proportion surviving",
         title = sprintf("Kaplan–Meier, log-rank χ² = %.2f, p = %.2g", test$chisq, p),
         subtitle = "Bands are 95% confidence intervals, + marks censoring") +
    theme_minimal(base_size = 13) +
    theme(legend.position = "inside", legend.position.inside = c(0.98, 0.98), legend.justification = c(1, 1),
          legend.background = element_rect(fill = "white", colour = NA))
  ctx$plot(print(figure), alt = sprintf("Kaplan-Meier curves by %s, log-rank chi-square %.2f", by, test$chisq), width = 7.5, height = 5)
}
