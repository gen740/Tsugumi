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

# Emits nothing at 1 (the loop ends); otherwise the walk so far and the parity of its last value.
# A ulong field takes digits: a double such as 27 would be written as 27.0, which the type rejects.
whole <- function(x) sprintf("%.0f", x)

main <- function(inputs) {
  outputs <- list()
  a <- arrival(inputs)
  if (a$n == 0) stop("Collatz is defined for positive integers")
  if (a$n == 1) return(invisible())
  outputs[["value"]] <- list(value = whole(a$n), path = I(whole(a$path)))

  outputs
}
