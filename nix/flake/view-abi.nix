{ inputs, ... }:
{
  perSystem =
    { pkgs, ... }:
    {
      checks.view-abi =
        pkgs.runCommand "tsugumi-view-abi"
          {
            nativeBuildInputs = [
              (pkgs.python3.withPackages (ps: [ ps.pyyaml ]))
              pkgs.llvmPackages.clang-unwrapped
            ];
            TSUGUMI_WEBGPU_YML = "${inputs.webgpu-headers}/webgpu.yml";
            TSUGUMI_VIEW_HEADER = "${../../protocol}/include/tsugumi/tsugumi_view.h";
          }
          ''
            python3 ${../../ui/shared/dawn}/test_gen_webgpu.py -v
            python3 ${../../ui/shared/dawn}/gen_webgpu.py \
              --webgpu-yml "$TSUGUMI_WEBGPU_YML" --view-header "$TSUGUMI_VIEW_HEADER" \
              --layout-check layout.c
            mkdir -p pinned/webgpu freestanding
            ln -s ${inputs.webgpu-headers}/webgpu.h pinned/webgpu/webgpu.h
            printf '#define NAN __builtin_nanf("")\n' > freestanding/math.h
            clang --target=wasm32 -std=c11 -ffreestanding -fsyntax-only \
              -I freestanding -I pinned -I ${../../protocol}/include layout.c
            touch "$out"
          '';
    };
}
