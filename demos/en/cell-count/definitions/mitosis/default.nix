{ pkgs, inputs, params }:
# A real fluorescence image of human cell nuclei, fixed by its hash: fetched once
# into the Nix store, never during a run. Moffat et al., Cell 124(6), 2006; image
# by David Root, CC0, from the scikit-image data repository.
let
  image = pkgs.fetchurl {
    url = params.url;
    hash = params.hash;
  };
in
{
  run = ''
    echo "source: ${params.url}"
    echo "hash: ${params.hash}"
    echo "image: ${image} ($(wc -c < ${image}) bytes)"
    printf '{"image": {"path": "%s", "source": "%s"}}' "${image}" "${params.url}" > "$TSUGUMI_OUTPUT"
  '';
}
