{ pkgs, inputs, params }:
# The observation is fixed like a package: fetched once by its hash from the
# HEASARC mirror of the Chandra archive (explicit FTPS), then every run reads
# the same file from the Nix store. Changing obsid, file or hash refetches.
let
  obsid = params.obsid;
  last = builtins.substring (builtins.stringLength obsid - 1) 1 obsid;
  url = "ftp://heasarc.gsfc.nasa.gov/chandra/data/byobsid/${last}/${obsid}/primary/${params.file}";
  raw = pkgs.fetchurl {
    inherit url;
    curlOptsList = [ "--ssl-reqd" ];
    hash = params.hash;
  };
  events = pkgs.runCommand "casa-${obsid}-evt2.fits" { } "gunzip -c ${raw} > $out";
in
{
  run = ''
    echo "source: ${url}"
    echo "hash: ${params.hash}"
    echo "events: ${events} ($(stat -c %s ${events}) bytes)"
    printf '{"events": {"path": "%s", "obsid": "%s"}}' "${events}" "${obsid}" > "$TSUGUMI_OUTPUT"
  '';
}
