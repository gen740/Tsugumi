# Tsugumi R Card SDK. A Card provides `main(ctx)` in src/main.R:
#
#   main <- function(ctx) {
#     n <- ctx$input("number")$value
#     ctx$output("result", list(value = n * ctx$params$factor))
#     ctx$plot(hist(rnorm(100)), alt = "a histogram")
#   }
#
# Port messages and params are lists, read from and written as JSON (the worker
# converts them to and from FlatBuffers by the port's type). A length-1 vector
# is written as a scalar: wrap a vector field in I() to keep it an array.
# An integer from 2^53 to 2^63, which a double cannot hold, is read as a string
# and written back from one (jsonlite rounds larger ones to a double). NA and
# NaN are written as null.
# A Card has one display: the last ctx$display_* or ctx$plot of the invocation.

ctx <- local({
  written <- list()
  params_json <- Sys.getenv("TSUGUMI_PARAMS_JSON", "{}")

  port_env <- function(prefix, port) Sys.getenv(paste0(prefix, toupper(port)), NA_character_)
  to_json <- function(x) jsonlite::toJSON(x, auto_unbox = TRUE, digits = I(17), always_decimal = TRUE, null = "null", na = "null")
  from_json <- function(x) jsonlite::fromJSON(x, simplifyVector = TRUE, bigint_as_char = TRUE)

  show <- function(member) {
    next_file <- paste0(Sys.getenv("TSUGUMI_DISPLAY"), ".new")
    writeLines(to_json(member), next_file, useBytes = TRUE)
    file.rename(next_file, Sys.getenv("TSUGUMI_DISPLAY"))
    invisible(NULL)
  }

  self <- list(
    params = from_json(params_json),
    output_directory = Sys.getenv("TSUGUMI_OUTPUT_DIR"),
    temporary_directory = Sys.getenv("TSUGUMI_TMP_DIR"),

    # The message of an input port, as a list.
    input = function(port) {
      file <- port_env("JSON_", port)
      if (is.na(file)) stop("no input port named ", port)
      from_json(file)
    },
    # The path a File or Directory input carries.
    path = function(port) {
      p <- port_env("FILE_", port)
      if (is.na(p)) p <- port_env("DIR_", port)
      if (is.na(p)) stop("no File or Directory input port named ", port)
      p
    },
    output = function(port, message) {
      written[[port]] <<- message
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
    display_module = function(name, data) show(list(module = list(name = name, data = data))),
    # Draws `expr` (base graphics or a ggplot) as the Card's image.
    plot = function(expr, alt = "", width = 8, height = 5, res = 160) {
      file <- tempfile(fileext = ".png", tmpdir = Sys.getenv("TSUGUMI_TMP_DIR"))
      grDevices::png(file, width = width, height = height, units = "in", res = res, type = "cairo")
      tryCatch({
        drawn <- withVisible(expr)
        if (drawn$visible && inherits(drawn$value, "ggplot")) print(drawn$value)
      }, finally = grDevices::dev.off())
      show(list(image = list(path = file, alt = alt)))
    }
  )
  self$finish <- function() {
    if (length(written)) writeLines(to_json(written), Sys.getenv("TSUGUMI_OUTPUT"), useBytes = TRUE)
  }
  self
})

source(file.path(commandArgs(TRUE)[1], "main.R"), keep.source = FALSE)
main(ctx)
ctx$finish()
