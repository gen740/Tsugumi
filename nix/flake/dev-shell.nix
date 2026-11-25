# devShells.default (`nix develop`): what builds the core (taken from its
# package), and the tools of the tests, the Web UI and Card formatting.
{ inputs, ... }:
{
  perSystem =
    {
      pkgs,
      lib,
      config,
      ...
    }:
    let
      core = config.packages.tsugumid;
    in
    {
      # clang as for the package (GCC 16 takes very long on the core's sources).
      devShells.default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) (
        {
          inputsFrom = [ core ];
          packages =
            with pkgs;
            [
              gtest
              emscripten
              git
              ruby
              (python3.withPackages (ps: [
                ps.flatbuffers
                ps.pytest
                ps.websockets
                ps.matplotlib
                ps.plotly
                ps.altair
                # ui/shared/dawn/gen_webgpu.py reads webgpu.yml.
                ps.pyyaml
              ]))
              jq
              clang-tools
              nixfmt
              # The web UI (web/) and its contract test with the Swift Agent.
              nodejs
            ]
            ++ lib.optionals stdenv.hostPlatform.isLinux [ bubblewrap ];
          TSUGUMI_DEMOS_DIR = config.packages.demos;
          MACOSX_DEPLOYMENT_TARGET = "26.0";
          # webgpu.yml for the dawn view generator (ui/shared/dawn).
          TSUGUMI_WEBGPU_HEADERS = inputs.webgpu-headers;
        }
        // core.devEnv
        // lib.optionalAttrs pkgs.stdenv.hostPlatform.isDarwin {
          TSUGUMI_WASMTIME_LICENSE = "${pkgs.wasmtime.src}/LICENSE";
          TSUGUMI_TEMPLATE_GIT_SOURCE = pkgs.git.src;
          TSUGUMI_TEMPLATE_GIT_SOURCE_URL = builtins.head pkgs.git.src.urls;
        }
      );
    };
}
