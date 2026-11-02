# tsugumid, tsugumi-worker and the installed Card SDK.
{ lib, stdenv, cmake, ninja, pkg-config, flatbuffers, sqlite, bash, coreutils, findutils, gnugrep
, gnused, gawk, gnutar, gzip, bubblewrap, self, cardNixpkgs, hosq }:
let
  # Tools on PATH for Card shell steps (preScript/script/postScript).
  cardTools = [ bash coreutils findutils gnugrep gnused gawk gnutar gzip ];
in
stdenv.mkDerivation {
  pname = "tsugumid";
  version = "1.0.0";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../CMakeLists.txt ../cmake ../core ../sdk ../protocol ../tests/cpp ];
  };
  nativeBuildInputs = [ cmake ninja pkg-config flatbuffers ];
  buildInputs = [ flatbuffers sqlite ];
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
