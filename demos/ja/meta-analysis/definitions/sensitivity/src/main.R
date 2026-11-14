suppressPackageStartupMessages(library(metafor))
library(ggplot2)

# What moves the pooled estimate: dropping one trial, or changing the tau^2 estimator.
main <- function(inputs, report, run) {
  outputs <- list()
  d <- read.csv(inputs$path("trials"))
  methods <- inputs$params$estimators
  fit <- function(x, method) {
    m <- rma(yi, vi, data = x, method = method)
    data.frame(rr = exp(as.numeric(coef(m))), lower = exp(m$ci.lb), upper = exp(m$ci.ub), tau2 = m$tau2, I2 = m$I2)
  }
  loo <- function(method, label) do.call(rbind, lapply(seq_len(nrow(d)), function(i)
    cbind(panel = label, name = sprintf("%2d %s", d$trial[i], d$author[i]), left_out = d$trial[i], fit(d[-i, ], method))))
  leave <- rbind(loo("EE", "1 試験を除く: 固定効果"), loo("DL", "1 試験を除く: 変量効果 DL"))
  estimators <- do.call(rbind, lapply(methods, function(k) cbind(panel = "推定量を変える (全試験)", name = k, left_out = NA, fit(d, k))))
  rows <- rbind(leave, estimators)
  file <- file.path(run$output_directory, "sensitivity.csv")
  write.csv(rows, file, row.names = FALSE)
  outputs[["sensitivity"]] <- list(value = file)

  full <- data.frame(panel = c("1 試験を除く: 固定効果", "1 試験を除く: 変量効果 DL"), rr = c(fit(d, "EE")$rr, fit(d, "DL")$rr))
  span <- function(x) sprintf("%.2f〜%.2f", min(x$rr), max(x$rr))
  names_order <- c(rev(unique(leave$name)), rev(methods))
  rows$name <- factor(rows$name, levels = names_order)
  rows$panel <- factor(rows$panel, levels = c(full$panel, "推定量を変える (全試験)"))
  figure <- ggplot(rows, aes(rr, name)) +
    geom_vline(data = full, aes(xintercept = rr), linetype = "dashed", colour = "grey40") +
    geom_errorbar(aes(xmin = lower, xmax = upper), width = 0, orientation = "y", colour = "#0072B2", linewidth = 0.6) +
    geom_point(colour = "#0072B2", size = 1.8) +
    facet_wrap(~ panel, scales = "free_y", nrow = 1) +
    scale_x_log10(breaks = c(0.4, 0.5, 0.7, 1)) +
    labs(x = "併合リスク比 (対数軸、横線は 95% 信頼区間、破線は全試験の値)", y = NULL,
         title = sprintf("併合リスク比の動く幅: 除外 (固定効果) %s, 除外 (変量効果) %s, 推定量 %s",
                         span(leave[leave$panel == full$panel[1], ]), span(leave[leave$panel == full$panel[2], ]), span(estimators))) +
    theme_minimal(base_size = 11) +
    theme(panel.grid.minor = element_blank())
  report$figure(print(figure), alt = "Leave-one-out and estimator comparison of the pooled risk ratio", width = 10, height = 4.8)

  outputs
}
