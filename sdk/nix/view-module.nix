# Bundle local ES imports and binary assets once, as an immutable view directory.
{ pkgs }:
{
  src,
  entry ? "index.js",
  name ? "tsugumi-view",
  minify ? true,
}:
pkgs.runCommand name { nativeBuildInputs = [ pkgs.esbuild ]; } ''
  mkdir -p $out
  esbuild ${src}/${entry} --bundle --format=esm --platform=browser --target=es2022 \
    --loader:.umd=text --loader:.wasm=binary --loader:.png=dataurl --loader:.jpg=dataurl --loader:.svg=dataurl \
    ${pkgs.lib.optionalString minify "--minify"} --outfile=$out/index.js
''
