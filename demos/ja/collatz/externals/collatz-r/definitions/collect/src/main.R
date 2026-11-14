# R keeps integers exactly only as doubles, up to 2^53. A larger value arrives as a string (the worker never
# rounds it for us): the Card fails rather than answer with a rounded number.
exact <- function(x, what = "a value") {
  if (is.character(x)) stop(what, " = ", x, " is not below 2^53, which R's doubles cannot hold exactly")
  x
}

# The loop delivers a merge union tagged with the arrival port: `init` carries a Number, `half` and `triple`
# a Walk (the value and every value before it, so that no Card has to remember anything between calls).
arrival <- function(inputs) {
  m <- inputs$arrival
  n <- exact(m$value$value, "n")
  list(n = n, path = if (m$value_type == "init") n else exact(m$value$path, "the sequence"))
}

# A ulong field takes digits: a double such as 27 would be written as 27.0, which the type rejects.
whole <- function(x) sprintf("%.0f", x)

main <- function(inputs, report) {
  outputs <- list()
  a <- arrival(inputs)
  report$text(paste0(length(a$path), " values: ", paste(format(a$path, scientific = FALSE, trim = TRUE), collapse = " ")))
  if (a$n == 1) outputs[["sequence"]] <- list(values = I(whole(a$path)))

  outputs
}
