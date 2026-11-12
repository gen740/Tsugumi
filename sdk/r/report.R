# A fresh R process renders one report invocation. This uses the same JSON and
# File/Directory environment contract as bash Cards; it does not need an R worker.
args <- commandArgs(TRUE)
src <- normalizePath(args[[1]], mustWork = TRUE)
entry <- args[[2]]
engine <- args[[3]]
if (grepl("(^/|(^|/)\\.\\.(/|$))", entry)) stop("report entry must stay inside src")
work <- tempfile("report-", tmpdir = Sys.getenv("TSUGUMI_TMP_DIR"))
dir.create(work, recursive = TRUE)
files <- list.files(src, all.files = TRUE, no.. = TRUE, full.names = TRUE)
files <- files[!grepl("(^|/)(_freeze|\\.quarto|.*_cache)$", files)]
if (length(files) && !all(file.copy(files, work, recursive = TRUE, copy.mode = FALSE))) stop("could not copy report sources")
setwd(work)
if (!file.exists(entry)) stop("missing report entry: ", entry)

read_json <- function(value) jsonlite::fromJSON(value, simplifyVector = TRUE, bigint_as_char = TRUE)
env <- Sys.getenv()
input_names <- names(env)[startsWith(names(env), "JSON_")]
inputs <- setNames(lapply(unname(env[input_names]), read_json), tolower(substring(input_names, 6)))
path_names <- names(env)[grepl("^(FILE_|DIR_)", names(env))]
paths <- setNames(as.list(unname(env[path_names])), tolower(sub("^(FILE_|DIR_)", "", path_names)))
context <- list(inputs = inputs, paths = paths, params = read_json(Sys.getenv("TSUGUMI_PARAMS_JSON", "{}")))
parameters <- list(tsugumi = context)
json <- function(x) jsonlite::toJSON(x, auto_unbox = TRUE, digits = I(17), null = "null", na = "null")
writeLines(json(parameters), "tsugumi-params.json", useBytes = TRUE)
message("Rendering src/", entry, " (chunk labels and source lines follow)")

if (engine == "rmarkdown") {
  knitr::opts_hooks$set(cache = function(options) { options$cache <- FALSE; options },
                      error = function(options) { options$error <- FALSE; options })
  knitr::opts_chunk$set(cache = FALSE, error = FALSE, warning = TRUE, message = TRUE)
  rmarkdown::render(entry, output_format = rmarkdown::html_document(self_contained = TRUE, mathjax = NULL),
                    output_file = "index.html", output_dir = work, knit_root_dir = work,
                    runtime = "static", params = parameters, envir = new.env(parent = globalenv()), quiet = FALSE)
} else if (engine == "quarto") {
  # Quarto resolves --output from its working directory. Keep generated assets
  # beside a nested entry, and override project output-dir inside this invocation.
  setwd(dirname(entry))
  status <- system2(Sys.getenv("TSUGUMI_QUARTO"), c("render", shQuote(basename(entry)), "--to", "html",
                    "--output", "index.html", "--output-dir", ".",
                    "--execute", "--no-cache", "--embed-resources",
                    "--execute-params", shQuote(file.path(work, "tsugumi-params.json")), "-M", "engine:knitr", "-M", "execute.error:false"))
  setwd(work)
  if (status != 0) stop("Quarto failed for src/", entry, " (exit ", status, ")")
} else stop("unknown report engine: ", engine)

html <- file.path(dirname(entry), "index.html")
if (engine == "rmarkdown") html <- "index.html"
if (!file.exists(html)) stop("renderer did not produce ", html)
destination <- file.path(Sys.getenv("TSUGUMI_OUTPUT_DIR"), "report")
if (!nzchar(Sys.getenv("TSUGUMI_OUTPUT_DIR"))) stop("report Card requires filesystem access")
dir.create(destination, recursive = TRUE)
# Preserve source, parameter provenance and related assets with the rendered HTML.
if (!all(file.copy(list.files(work, all.files = TRUE, no.. = TRUE, full.names = TRUE), destination,
                   recursive = TRUE, copy.mode = FALSE))) stop("could not save report artifacts")
writeLines(json(list(report = list(value = destination), html = list(value = file.path(destination, html)))),
           Sys.getenv("TSUGUMI_OUTPUT"), useBytes = TRUE)
writeLines(json(list(markdown = "Report rendered. Open **Artifacts** to preview the saved HTML and inspect its source and assets.")),
           Sys.getenv("TSUGUMI_DISPLAY"), useBytes = TRUE)
