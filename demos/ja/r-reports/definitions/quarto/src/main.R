main <- function(input) {
  report <- artifact_dir()
  files <- report$path
  source <- "report.qmd"
  source_path <- file.path(dirname(getSrcFilename(main, full.names = TRUE)), source)
  stopifnot(file.copy(source_path, file.path(files, source)))
  context <- list(inputs = list(data = input$data), params = params)
  jsonlite::write_json(context, file.path(files, "tsugumi-params.json"), auto_unbox = TRUE)
  parameter_file <- tempfile(fileext = ".yml")
  yaml::write_yaml(list(tsugumi = context), parameter_file)
  processx::run("quarto", c("render", source, "--to", "html",
      "--output", "index.html", "--execute-params", parameter_file),
      wd = files, echo = TRUE)
  html <- artifact_upload(file.path(files, "index.html"))
  # TODO(open): Port the demo display.
  list(report = report$artifact, html = html)
}
