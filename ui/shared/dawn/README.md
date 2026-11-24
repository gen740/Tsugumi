# WebGPU imports for Dawn views

`gen_webgpu.py` reads the pinned
[webgpu-headers schema](https://github.com/webgpu-native/webgpu-headers/tree/a1dc77409bb2b8bd80540cbf4b8db219e123e909)
and the `TSUGUMI_VIEW_WEBGPU_IMPORTS(X)` list in
[`tsugumi_view.h`](../../../protocol/include/tsugumi/tsugumi_view.h). It
computes the wasm32 layouts and follows the descriptor types from the selected
functions. Both hosts use those layouts to read the module's linear memory.

- `--cpp DIR` writes `webgpu_imports.gen.h`, `webgpu_imports.gen.cc`, and
  `webgpu_procs_dawn.gen.cc`. The decoder uses Dawn constants by name and
  calls the supplied procedure table. Recorder tests compile only the decoder.
- `--js FILE` writes an ES module and a companion `.d.ts`. The module translates
  the same descriptors to the browser's WebGPU API and exports the numeric
  texture formats that the view ABI accepts at startup.
- `--layout-check FILE` writes a C translation unit that checks field offsets,
  type sizes and alignments, enum and flag values, and sentinel values against
  the pinned C header when compiled for wasm32.

Generate the files with Python 3 and PyYAML. Set `WEBGPU_HEADERS` to a checkout
of the pinned webgpu-headers input and run these commands from the repository root.

```sh
python3 ui/shared/dawn/gen_webgpu.py \
  --webgpu-yml "$WEBGPU_HEADERS/webgpu.yml" \
  --view-header protocol/include/tsugumi/tsugumi_view.h \
  --cpp build-view/gen \
  --js ui/web/shared/gen/webgpu_imports.gen.js \
  --layout-check build-view/layout.c

mkdir -p build-view/pinned/webgpu build-view/freestanding
ln -s "$WEBGPU_HEADERS/webgpu.h" build-view/pinned/webgpu/webgpu.h
printf '#define NAN __builtin_nanf("")\n' > build-view/freestanding/math.h
clang --target=wasm32 -std=c11 -ffreestanding -fsyntax-only \
  -I build-view/freestanding -I build-view/pinned -I protocol/include build-view/layout.c
```

The Mac CMake target uses `tsugumi_generate_webgpu_imports` from
[`webgpu_gen.cmake`](webgpu_gen.cmake). Targets compiling the source list
declare a dependency on `tsugumi-webgpu-generated`. The Web build generates the
module under `ui/web/shared/gen`. Generated files belong to their build
outputs.

To extend the subset, add one `X(return_type, wgpuFunction, (arguments))` line
to the header with the signature from the pinned `webgpu.h`. The generator
reads the name and derives the Wasm signature and descriptor closure from YAML.
If the signature introduces an unsupported shape or an unmapped WebIDL value,
add its shared ABI model and conversion before enabling the import. Run the
layout check and the GPU-free tests after making the change.

```sh
TSUGUMI_WEBGPU_YML="$WEBGPU_HEADERS/webgpu.yml" \
  TSUGUMI_VIEW_HEADER=protocol/include/tsugumi/tsugumi_view.h \
  python3 ui/shared/dawn/test_gen_webgpu.py -v
node ui/web/tests/dawnImports.ts
```

The native recorder target is `tsugumi-view-decode-tests`. Its dependencies
are GoogleTest, the generator, and Dawn's headers.
