# Interactive point cloud

Run the `cloud` Card and open its `points` view. Drag to orbit the torus
knot and scroll to zoom. `count` sets the point count (1 to 2,000,000) and
`seed` changes the sample. The colour varies along the curve.

The C++ Card sends at most 1 MiB per message. Its local resident worker
replays the point cloud when you reopen the view. The view assembles the
chunks in Wasm memory, then uploads them to WebGPU and draws each point as a
small screen-aligned disc with depth testing.
