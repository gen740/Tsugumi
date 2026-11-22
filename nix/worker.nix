# Build the worker against the same package set and system as the Card.
{ pkgs }:
pkgs.stdenv.mkDerivation {
  name = "tsugumi-worker";
  src = ./.;
  nativeBuildInputs = [ pkgs.flatbuffers ];
  buildInputs = [ pkgs.flatbuffers ];
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p generated/tsugumi/protocol
    flatc --cpp --scoped-enums --gen-mutable --keep-prefix --include-prefix tsugumi/protocol \
      -I protocol -o generated/tsugumi/protocol protocol/*.fbs
    "$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough \
      -Iinclude -Igenerated src/native_main.cc \
      -lflatbuffers -pthread -ldl -o tsugumi-worker
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 tsugumi-worker "$out/bin/tsugumi-worker"
    runHook postInstall
  '';
}
