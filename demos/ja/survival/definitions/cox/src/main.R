library(survival)

# A Cox model on the chosen cohort with the covariates in the param.
main <- function(inputs) {
  workspace <- tempfile("artifact-")
  dir.create(workspace)
  outputs <- list()
  covariates <- params$covariates
  d <- read.csv(file.path(artifact_open(inputs$cohort), "cohort.csv"))
  formula <- as.formula(paste("Surv(time, event) ~", paste(covariates, collapse = " + ")), env = globalenv())
  fit <- coxph(formula, d, x = TRUE, y = TRUE, model = TRUE)
  s <- summary(fit)
  rows <- data.frame(term = rownames(s$coefficients), coef = s$coefficients[, "coef"], se = s$coefficients[, "se(coef)"],
                     hr = s$conf.int[, "exp(coef)"], lower = s$conf.int[, "lower .95"], upper = s$conf.int[, "upper .95"],
                     p = s$coefficients[, "Pr(>|z|)"], n = fit$n, events = fit$nevent)
  out <- file.path(workspace, "cox")
  dir.create(out)
  saveRDS(fit, file.path(out, "fit.rds"))
  write.csv(rows, file.path(out, "coefficients.csv"), row.names = FALSE)
  outputs[["fit"]] <- artifact_upload(out)
  # TODO(open): Port the demo display.

  outputs
}
