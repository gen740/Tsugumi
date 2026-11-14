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

# Emits nothing at 1 (the loop ends); otherwise the walk so far and the parity of its last value.
# A ulong field takes digits: a double such as 27 would be written as 27.0, which the type rejects.
whole <- function(x) sprintf("%.0f", x)

main <- function(inputs) {
  outputs <- list()
  a <- arrival(inputs)
  if (a$n == 0) stop("Collatz is defined for positive integers")
  if (a$n == 1) return(invisible())
  outputs[["value"]] <- list(value = whole(a$n), path = I(whole(a$path)))
  outputs[["even"]] <- list(value = a$n %% 2 == 0)

  outputs
}
