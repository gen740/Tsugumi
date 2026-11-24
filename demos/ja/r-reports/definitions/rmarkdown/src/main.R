main <- function(input) {
  report <- artifact_dir()
  files <- report$path
  source <- "report.Rmd"
  source_path <- file.path(dirname(getSrcFilename(main, full.names = TRUE)), source)
  stopifnot(file.copy(source_path, file.path(files, source)))
  context <- list(inputs = list(data = input$data), params = params)
  jsonlite::write_json(context, file.path(files, "tsugumi-params.json"), auto_unbox = TRUE)
  rmarkdown::render(file.path(files, source), output_file = "index.html",
                    params = list(tsugumi = context),
                    envir = new.env(parent = globalenv()))
  html <- artifact_upload(file.path(files, "index.html"))
  # TODO(open): Port the demo display.
  list(report = report$artifact, html = html)
}
