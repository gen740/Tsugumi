library(survival)

# A Cox model on the chosen cohort with the covariates in the param.
main <- function(ctx) {
  covariates <- ctx$params$covariates
  d <- read.csv(ctx$path("cohort"))
  formula <- as.formula(paste("Surv(time, event) ~", paste(covariates, collapse = " + ")), env = globalenv())
  fit <- coxph(formula, d, x = TRUE, y = TRUE, model = TRUE)
  s <- summary(fit)
  rows <- data.frame(term = rownames(s$coefficients), coef = s$coefficients[, "coef"], se = s$coefficients[, "se(coef)"],
                     hr = s$conf.int[, "exp(coef)"], lower = s$conf.int[, "lower .95"], upper = s$conf.int[, "upper .95"],
                     p = s$coefficients[, "Pr(>|z|)"], n = fit$n, events = fit$nevent)
  out <- file.path(ctx$output_directory, "cox")
  dir.create(out)
  saveRDS(fit, file.path(out, "fit.rds"))
  write.csv(rows, file.path(out, "coefficients.csv"), row.names = FALSE)
  ctx$output("fit", list(value = out))
  ctx$display_table(data.frame(
    Term = rows$term, HR = sprintf("%.3f", rows$hr), `95%CI` = sprintf("%.3f–%.3f", rows$lower, rows$upper),
    p = sprintf("%.2g", rows$p), n = rows$n, Deaths = rows$events, check.names = FALSE))
}
