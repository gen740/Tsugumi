# R keeps integers exactly only as doubles, up to 2^53; a larger one arrives as a string, and 3n+1 is checked
# before it is computed: the Card fails rather than answer with a rounded number.
exact <- function(x, what = "a value") {
  if (is.character(x)) stop(what, " = ", x, " is not below 2^53, which R's doubles cannot hold exactly")
  x
}

# A ulong field takes digits: a double such as 27 would be written as 27.0, which the type rejects.
whole <- function(x) sprintf("%.0f", x)

main <- function(inputs, report) {
  outputs <- list()
  walk <- inputs$input("number")
  n <- exact(walk$value, "n")
  half <- n %/% 2
  outputs[["next"]] <- list(value = whole(half), path = I(whole(c(exact(walk$path, "the sequence"), half))))

  outputs
}
