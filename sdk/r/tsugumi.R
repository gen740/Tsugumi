# Tsugumi R Card SDK. A Card provides `main(ctx)` in src/main.R:
#
#   main <- function(ctx) {
#     n <- ctx$input("number")$value
#     ctx$output("result", list(value = n * ctx$params$factor))
#     ctx$plot(hist(rnorm(100)), alt = "a histogram")
#   }
#
# The worker passes FlatBuffers through the Card ABI. Its in-memory adapter
# presents messages as lists without temporary JSON files. A length-1 vector
# is written as a scalar: wrap a vector field in I() to keep it an array.
# Integers too large for an exact R double are decimal strings; NA/NaN are null.
# A Card keeps no state by default: each invocation loads this file and the
# Card's source in a new interpreter. With reuseWorker: true the interpreter
# and module-level state persist. Each call gets a fresh ctx; display
# functions publish immediately.

.tsugumi_context <- function() {
  # FlatBuffers reads a union's discriminator before its value.
  order_unions <- function(x) {
    if (is.list(x) && !is.data.frame(x)) {
      x <- lapply(x, order_unions)
      if (!is.null(names(x))) x <- x[order(!endsWith(names(x), "_type"))]
    }
    x
  }
  to_json <- function(x) jsonlite::toJSON(order_unions(x), auto_unbox = TRUE, digits = I(17), always_decimal = TRUE, null = "null", na = "null")
  from_json <- function(x) jsonlite::fromJSON(x, simplifyVector = TRUE, bigint_as_char = TRUE)

  show <- function(member) {
    if (!.Call("tsugumi_display", as.character(to_json(member)))) stop("display was rejected by the worker")
    invisible(NULL)
  }

  self <- list(
    params = from_json(.Call("tsugumi_params")),
    run_id = .Call("tsugumi_info", 0L),
    card = .Call("tsugumi_info", 1L),
    invocation = .Call("tsugumi_invocation"),
    working_directory = .Call("tsugumi_info", 2L),
    temporary_directory = .Call("tsugumi_info", 3L),
    output_directory = .Call("tsugumi_info", 4L),
    cancelled = function() .Call("tsugumi_cancelled"),
    remaining_ms = function() .Call("tsugumi_remaining"),

    input = function(port) {
      value <- .Call("tsugumi_input", as.character(port))
      if (is.null(value)) return(NULL)
      from_json(value)
    },
    path = function(port) {
      if (!(.Call("tsugumi_input_kind", as.character(port)) %in% c(1L, 2L))) stop("no File or Directory input port named ", port)
      value <- self$input(port)$value
      if (!is.character(value) || length(value) != 1L) stop("no File or Directory input port named ", port)
      value
    },
    output = function(port, message) {
      if (!.Call("tsugumi_output", as.character(port), as.character(to_json(message)))) stop("output was rejected by the worker: ", port)
      invisible(NULL)
    },
    display_text = function(text) show(list(text = paste(text, collapse = "\n"))),
    display_markdown = function(text) show(list(markdown = paste(text, collapse = "\n"))),
    # A data.frame or matrix (`digits`: significant digits of numeric columns).
    display_table = function(table, digits = 4) {
      table <- as.data.frame(table, stringsAsFactors = FALSE)
      cells <- vapply(table, function(column) {
        if (is.numeric(column)) format(column, digits = digits, trim = TRUE) else as.character(column)
      }, character(nrow(table)))
      cells <- matrix(cells, nrow = nrow(table), ncol = ncol(table))
      rows <- lapply(seq_len(nrow(cells)), function(r) I(cells[r, ]))
      show(list(table = list(columns = I(names(table)), rows = rows)))
    },
    # A PNG or JPEG file.
    display_image = function(path, alt = "") show(list(image = list(path = path, alt = alt))),
    # An interactive plot: name is "vega-lite" or "plotly", data its spec as a list.
    display_module = function(name, data) show(list(module = list(module = name, config = data))),
    # Draws `expr` (base graphics or a ggplot) as the Card's image.
    plot = function(expr, alt = "", width = 8, height = 5, res = 160) {
      file <- tempfile(fileext = ".png", tmpdir = self$temporary_directory)
      grDevices::png(file, width = width, height = height, units = "in", res = res, type = "cairo")
      tryCatch({
        drawn <- withVisible(expr)
        if (drawn$visible && inherits(drawn$value, "ggplot")) print(drawn$value)
      }, finally = grDevices::dev.off())
      show(list(image = list(path = file, alt = alt)))
    }
  )
  self
}

.tsugumi_invoke <- function(hook) {
  tryCatch({
    ctx <- .tsugumi_context()
    if (hook) {
      if (exists("on_run_start", envir = .GlobalEnv, inherits = FALSE)) on_run_start(ctx)
    } else main(ctx)
    TRUE
  }, error = function(error) {
    .Call("tsugumi_fail", conditionMessage(error))
    FALSE
  })
}
