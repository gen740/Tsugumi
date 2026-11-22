# Python dependencies imported by the demo test runners.
{ python3 }:
python3.withPackages (ps: [
  ps.pytest
  ps.pytest-xdist
  ps.flatbuffers
  ps.websockets
  ps.numpy
])
