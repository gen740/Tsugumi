# devShells.default (`nix develop`): what builds the core (taken from its
# package), and the tools of the tests, the Web UI and Card formatting.
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
              # Use the SDK's Ruby for interpreter adapter tests on macOS too.
              ruby
              (python3.withPackages (ps: [
                ps.flatbuffers
                ps.pytest
                ps.websockets
                ps.matplotlib
                ps.plotly
                ps.altair
              ]))
              # Card shell steps use jq (the package gives them the same tools).
              jq
              # Source formatters used by ProjectFileFormat (also in the image).
              ruff
              nixfmt
              shfmt
              # The web UI (web/) and its contract test with the Swift Agent.
              nodejs
            ]
            ++ lib.optionals stdenv.hostPlatform.isLinux [ bubblewrap ];
          MACOSX_DEPLOYMENT_TARGET = "26.0";
        }
        // core.devEnv
      );
    };
}
