# tsugumid, tsugumi-worker and the installed Card SDK.
{ lib, stdenv, cmake, ninja, pkg-config, flatbuffers, sqlite, nix, bash, coreutils, findutils, gnugrep
, gnused, gawk, gnutar, gzip, jq, bubblewrap, self, cardNixpkgs, hosq }:
let
  # Tools on PATH for Card bash steps (preRun, a bash run, postRun).
  cardTools = [ bash coreutils findutils gnugrep gnused gawk gnutar gzip jq ];
in
stdenv.mkDerivation {
  pname = "tsugumid";
  version = "1.0.0";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../CMakeLists.txt ../cmake ../core ../sdk ../protocol ../tests/cpp ];
  };
  nativeBuildInputs = [ cmake ninja pkg-config flatbuffers ];
  # The embedded Nix evaluator: the same Nix as the environment's daemon.
  buildInputs = [ flatbuffers sqlite nix.libs.nix-expr nix.libs.nix-store nix.libs.nix-fetchers nix.libs.nix-util ];
  cmakeFlags = [
    "-DHOSQ_INCLUDE_DIR=${hosq}/include"
    "-DTSUGUMI_BUILD_TESTS=OFF"
    "-DTSUGUMI_CARD_NIXPKGS_REV=${cardNixpkgs.rev}"
    "-DTSUGUMI_CARD_NIXPKGS_NARHASH=${cardNixpkgs.narHash}"
    "-DTSUGUMI_BASH=${bash}/bin/bash"
    "-DTSUGUMI_CARD_PATH=${lib.makeBinPath cardTools}"
  ] ++ lib.optionals stdenv.hostPlatform.isLinux [ "-DTSUGUMI_BWRAP=${bubblewrap}/bin/bwrap" ];
  meta = {
    description = "Tsugumi core daemon and Card worker";
    mainProgram = "tsugumid";
    platforms = [ "aarch64-linux" "aarch64-darwin" ];
  };
}
