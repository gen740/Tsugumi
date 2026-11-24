{ params, inputs }:
# A real fluorescence image of human cell nuclei, fixed by its hash: fetched once
# into the Nix store, never during a run. Moffat et al., Cell 124(6), 2006; image
# by David Root, CC0, from the scikit-image data repository.
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/input> { inherit pkgs; };
  image = pkgs.fetchurl {
    url = params.url;
    hash = params.hash;
  };
in
{
  run = sdk.input {
    outputs = ./_outputs.fbs;
    values.image = { file = sdk.artifact image; source = params.url; };
  };
}
