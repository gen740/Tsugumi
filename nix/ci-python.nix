{ python3 }:
let
  # SDK behavior is checked by this repository. The plotting packages' upstream
  # suites pull unrelated build dependencies, including DuckDB through narwhals.
  noCheck = p: p.overridePythonAttrs { doCheck = false; };
  python = python3.override {
    self = python;
    packageOverrides = _: super: {
      narwhals = noCheck super.narwhals;
      plotly = noCheck super.plotly;
      altair = noCheck super.altair;
    };
  };
in
python.withPackages (ps: [
  ps.pytest
  ps.pytest-xdist
  ps.flatbuffers
  ps.websockets
  ps.matplotlib
  ps.plotly
  ps.altair
  ps.vl-convert-python
  ps.marimo
])
