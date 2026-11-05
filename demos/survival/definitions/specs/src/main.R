library(survival)
library(ggplot2)

# The same hazard ratio under every combination of who is included and what is adjusted for.
main <- function(ctx) {
  subsets <- ctx$params$subsets
  adjustments <- ctx$params$adjustments
  exposure <- ctx$params$exposure
  d <- read.csv(ctx$path("lung"))

  one <- function(subset_text, adjustment) {
    x <- subset(d, eval(parse(text = subset_text)))
    right <- paste(c(exposure, if (nzchar(adjustment)) adjustment), collapse = " + ")
    fit <- coxph(as.formula(paste("Surv(time, event) ~", right)), x)
    s <- summary(fit)
    data.frame(subset = subset_text, adjustment = if (nzchar(adjustment)) adjustment else "調整なし", n = fit$n, events = fit$nevent,
               hr = s$conf.int[exposure, "exp(coef)"], lower = s$conf.int[exposure, "lower .95"], upper = s$conf.int[exposure, "upper .95"])
  }
  rows <- do.call(rbind, lapply(subsets, function(s) do.call(rbind, lapply(adjustments, function(a) one(s, a)))))
  file <- file.path(ctx$output_directory, "specifications.csv")
  write.csv(rows, file, row.names = FALSE)
  ctx$output("specifications", list(value = file))

  rows$subset <- factor(ifelse(rows$subset == "TRUE", "全員", rows$subset), levels = ifelse(subsets == "TRUE", "全員", subsets))
  labelled <- ifelse(adjustments == "", "調整なし", adjustments)
  rows$adjustment <- factor(rows$adjustment, levels = rev(labelled))
  rows$text <- sprintf("%.2f (%.2f〜%.2f)  n=%d", rows$hr, rows$lower, rows$upper, rows$n)
  right_edge <- max(rows$upper) * 2.4
  figure <- ggplot(rows, aes(hr, adjustment)) +
    geom_vline(xintercept = 1, colour = "grey50") +
    geom_errorbar(aes(xmin = lower, xmax = upper), width = 0, orientation = "y", colour = "#0072B2", linewidth = 0.7) +
    geom_point(colour = "#0072B2", size = 2) +
    geom_text(aes(x = right_edge, label = text), hjust = 1, size = 3.1, colour = "grey20") +
    facet_grid(subset ~ ., switch = "y") +
    scale_x_log10(breaks = c(0.2, 0.5, 1, 2)) +
    labs(x = paste0(exposure, " のハザード比 (対数軸、横線は 95% 信頼区間)"), y = NULL,
         title = "誰を含め、何で調整するかでハザード比はどう動くか") +
    theme_minimal(base_size = 11) +
    theme(strip.placement = "outside", strip.text.y.left = element_text(angle = 0, face = "bold"),
          panel.grid.minor = element_blank(), panel.spacing.y = unit(6, "pt"))
  ctx$plot(print(figure), alt = "Hazard ratio across specifications", width = 8.5, height = 1 + 0.27 * nrow(rows) + 0.1 * length(subsets))
}
