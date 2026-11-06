suppressPackageStartupMessages(library(metafor))
library(ggplot2)
library(ggrepel)

# Meta-regression: does the effect change with the moderator?
main <- function(ctx) {
  name <- ctx$params$moderator
  method <- ctx$params$method
  d <- read.csv(ctx$path("trials"))
  d$x <- d[[name]]
  overall <- rma(yi, vi, data = d, method = method)
  m <- rma(yi, vi, mods = ~ x, data = d, method = method)
  rows <- data.frame(moderator = name, method = method, intercept = coef(m)[1], slope = coef(m)[2], slope_se = m$se[2],
                     QM = m$QM, QM_p = m$QMp, tau2 = m$tau2, tau2_without = overall$tau2, R2 = m$R2, I2 = m$I2)
  file <- file.path(ctx$output_directory, "regression.csv")
  write.csv(rows, file, row.names = FALSE)
  ctx$output("regression", list(value = file))
  print(rows)

  grid <- seq(min(d$x), max(d$x), length.out = 100)
  band <- as.data.frame(predict(m, newmods = grid))
  band$x <- grid
  d$weight <- weights(m)
  figure <- ggplot(d, aes(x, yi)) +
    geom_hline(yintercept = 0, colour = "grey60") +
    geom_ribbon(data = band, aes(x, ymin = ci.lb, ymax = ci.ub), inherit.aes = FALSE, fill = "#0072B2", alpha = 0.15) +
    geom_line(data = band, aes(x, pred), inherit.aes = FALSE, colour = "#0072B2", linewidth = 0.9) +
    geom_point(aes(size = weight), colour = "#D55E00", alpha = 0.75, show.legend = FALSE) +
    geom_text_repel(aes(label = trial), size = 3.4, colour = "grey20", box.padding = 0.55, seed = 1, min.segment.length = Inf) +
    scale_size_area(max_size = 9) +
    scale_y_continuous(sec.axis = sec_axis(transform = exp, name = "リスク比", breaks = c(0.1, 0.2, 0.5, 1, 2))) +
    labs(x = if (name == "ablat") "試験地の緯度の絶対値 (度)" else name, y = "log リスク比",
         title = sprintf("傾き %.4f (p = %.2g)。τ² は %.3f → %.3f、説明された割合 R² = %.0f%%", m$b[2], m$pval[2], overall$tau2, m$tau2, m$R2),
         subtitle = "点の大きさは重み、数字は試験番号、帯は 95% 信頼区間") +
    theme_minimal(base_size = 12)
  ctx$plot(print(figure), alt = sprintf("Meta-regression on %s, slope %.4f", name, m$b[2]), width = 7.5, height = 5)
}
