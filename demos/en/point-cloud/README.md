# Interactive point cloud

Run the `cloud` Card to open its `points` window. Drag to orbit the torus
knot and scroll to zoom. `count` sets the point count (1 to 2,000,000) and
`seed` changes the sample. The colour varies along the curve.

The C++ Card sends at most 1 MiB per message. The view assembles the
chunks in Wasm memory, then uploads them to WebGPU and draws each point as a
small screen-aligned disc with depth testing.
