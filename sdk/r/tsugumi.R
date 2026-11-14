# Tsugumi R Cards define main(inputs[, report[, run]]).
# Return a named list of output messages; File/Directory values carry paths.
# Inputs use inputs$port and inputs$params. Report methods publish immediately.
# Wrap length-one vectors in I() to retain their array shape in FlatBuffers.

.tsugumi_from_json <- function(text) {
  # Only bare numeric tokens change; quoted strings keep their meaning. '$'
  # cannot occur in a FlatBuffers field name, so this internal tag is unambiguous.
  tokens <- gregexpr('"(?:[^"\\\\]|\\\\.)*"|-?Infinity|NaN', text, perl = TRUE)
  values <- regmatches(text, tokens)[[1]]
  replacement <- vapply(values, function(value) {
    if (startsWith(value, '"')) value else paste0('{"$tsugumi_number":"', value, '"}')
  }, character(1))
  regmatches(text, tokens) <- list(replacement)
  restore <- function(value) {
    if (is.list(value) && identical(names(value), "$tsugumi_number"))
      return(switch(value[[1]], `NaN` = NaN, Infinity = Inf, `-Infinity` = -Inf))
    if (is.list(value)) {
      value <- lapply(value, restore)
      if (is.null(names(value)) && length(value) && all(vapply(value, function(x) is.atomic(x) && length(x) == 1L, logical(1))))
        value <- unlist(value, use.names = FALSE)
    }
    value
  }
  restore(jsonlite::fromJSON(text, simplifyVector = FALSE, bigint_as_char = TRUE))
}

.tsugumi_arguments <- function(scope) {
  check <- function() { if (!isTRUE(scope$active)) stop("Card call has expired") }
  # FlatBuffers reads a union's discriminator before its value.
  order_unions <- function(x) {
    if (is.numeric(x) && any(is.na(x) & !is.nan(x)))
      stop("NA has no FlatBuffers numeric value; use NaN or NULL for an absent optional scalar")
    if (is.list(x) && !is.data.frame(x)) {
      x <- lapply(x, order_unions)
      if (!is.null(names(x))) x <- x[order(!endsWith(names(x), "_type"))]
    }
    x
  }
  to_json <- function(x) jsonlite::toJSON(order_unions(x), auto_unbox = TRUE, digits = I(17), always_decimal = TRUE, null = "null", na = "string")
  .tsugumi_json <<- to_json
  from_json <- .tsugumi_from_json

  show <- function(member) {
    if (!.Call("tsugumi_display", as.character(to_json(member)))) stop("display was rejected by the worker")
    invisible(NULL)
  }

  run <- list(
    run_id = .Call("tsugumi_info", 0L), card = .Call("tsugumi_info", 1L),
    invocation = .Call("tsugumi_invocation"),
    working_directory = .Call("tsugumi_info", 2L),
    temporary_directory = .Call("tsugumi_info", 3L),
    output_directory = .Call("tsugumi_info", 4L),
    cancelled = function() { check(); .Call("tsugumi_cancelled") },
    remaining_ms = function() { check(); .Call("tsugumi_remaining") }
  )
  class(run) <- "Run"
  read <- function(port) {
    value <- .Call("tsugumi_input", as.character(port))
    if (is.null(value)) NULL else from_json(value)
  }
  inputs <- structure(list(params = from_json(.Call("tsugumi_params")), read = read), class = "TsugumiInputs")
  report <- list(
    text = function(text) show(list(text = paste(text, collapse = "\n"))),
    markdown = function(text) show(list(markdown = paste(text, collapse = "\n"))),
    # A data.frame or matrix (`digits`: significant digits of numeric columns).
    table = function(table, digits = 4) {
      table <- as.data.frame(table, stringsAsFactors = FALSE)
      cells <- vapply(table, function(column) {
        if (is.numeric(column)) format(column, digits = digits, trim = TRUE) else as.character(column)
      }, character(nrow(table)))
      cells <- matrix(cells, nrow = nrow(table), ncol = ncol(table))
      rows <- lapply(seq_len(nrow(cells)), function(r) I(cells[r, ]))
      show(list(table = list(columns = I(names(table)), rows = rows)))
    },
    # A PNG or JPEG file.
    image = function(path, alt = "") show(list(image = list(path = path, alt = alt))),
    # An interactive plot: name is "vega-lite" or "plotly", data its spec as a list.
    module = function(name, data) show(list(module = list(module = name, config = data))),
    # Draws `expr` (base graphics or a ggplot) as the Card's image.
    figure = function(expr, alt = "", width = 8, height = 5, res = 160) {
      file <- tempfile(fileext = ".png", tmpdir = run$temporary_directory)
      grDevices::png(file, width = width, height = height, units = "in", res = res, type = "cairo")
      tryCatch({
        drawn <- withVisible(expr)
        if (drawn$visible && inherits(drawn$value, "ggplot")) print(drawn$value)
      }, finally = grDevices::dev.off())
      show(list(image = list(path = file, alt = alt)))
    },
    log = function(message) { .Call("tsugumi_log", as.character(message)); invisible(NULL) }
  )
  class(report) <- "Report"
  list(inputs, report, run)
}

`$.TsugumiInputs` <- function(x, name) {
  if (name == "params") return(unclass(x)[["params"]])
  if (name == "input") return(unclass(x)[["read"]])
  if (name == "path") return(function(port) {
    if (!(.Call("tsugumi_input_kind", as.character(port)) %in% c(1L, 2L))) stop("no File or Directory input port named ", port)
    value <- unclass(x)[["read"]](port)$value
    if (!is.character(value) || length(value) != 1L) stop("no File or Directory input port named ", port)
    value
  })
  unclass(x)[["read"]](name)
}
`$<-.TsugumiInputs` <- function(x, name, value) stop("inputs are read-only")

.tsugumi_outputs <- function(result) {
  if (is.function(result)) {
    repeat { batch <- result(); if (is.null(batch)) break; .tsugumi_outputs(batch) }
    return(invisible(NULL))
  }
  if (!is.null(result)) {
    if (is.list(result) && length(result) == 0L) return(invisible(NULL))
    if (!is.list(result) || is.null(names(result))) stop("return a named list of output ports")
    for (port in names(result)) {
      message <- result[[port]]
      if (!.Call("tsugumi_output", port, as.character(.tsugumi_json(message)))) stop("output was rejected: ", port)
    }
  }
  invisible(NULL)
}

.tsugumi_invoke <- function(hook) {
  scope <- new.env(parent = emptyenv())
  scope$active <- TRUE
  on.exit(scope$active <- FALSE)
  tryCatch({
    arguments <- .tsugumi_arguments(scope)
    function_ <- if (hook) {
      if (exists("on_run_start", envir = .GlobalEnv, inherits = FALSE)) on_run_start else NULL
    } else main
    if (!is.null(function_)) {
      count <- length(formals(function_))
      if (!(count %in% 1:3)) stop("Card functions take inputs, optionally report and run")
      .tsugumi_outputs(do.call(function_, arguments[seq_len(count)]))
    }
    if (!hook && !.Call("tsugumi_finish_outputs")) stop("required outputs were not emitted")
    TRUE
  }, error = function(error) {
    .Call("tsugumi_fail", conditionMessage(error))
    FALSE
  })
}
