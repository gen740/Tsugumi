# R keeps integers exactly only as doubles, up to 2^53. A larger value arrives as a string (the worker never
# rounds it for us): the Card fails rather than answer with a rounded number.
exact <- function(x, what = "a value") {
  if (is.character(x)) stop(what, " = ", x, " is not below 2^53, which R's doubles cannot hold exactly")
  x
}

# Initial values start a sequence. Feedback carries the sequence so far.
arrival <- function(inputs) {
  value <- inputs$arrival$value
  if (!is.null(value$seed)) {
    n <- exact(value$seed, "n")
    return(list(n = n, path = n))
  }
  list(n = exact(value$value, "n"), path = exact(value$path, "the sequence"))
}

# A ulong field takes digits: a double such as 27 would be written as 27.0, which the type rejects.
whole <- function(x) sprintf("%.0f", x)

main <- function(inputs) {
  outputs <- list()
  a <- arrival(inputs)
  # TODO(open): Port the demo display.
  if (a$n == 1) outputs[["sequence"]] <- list(values = I(whole(a$path)))

  outputs
}
