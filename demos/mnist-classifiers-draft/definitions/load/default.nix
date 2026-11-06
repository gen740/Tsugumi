{ pkgs, inputs, params }:
# MNIST is fixed like a package: fetched once by its hash, then every run reads
# the same files from the Nix store. No network during a run.
let
  sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
  mirror = "https://ossci-datasets.s3.amazonaws.com/mnist";
  fetch = name: hash: pkgs.fetchurl { url = "${mirror}/${name}.gz"; inherit hash; };
  mnist = pkgs.runCommand "mnist-idx" { } ''
    mkdir $out
    gunzip -c ${fetch "train-images-idx3-ubyte" "sha256-RA/Kv3PMVG+iFHXoHqNwJlYF9WviEKQCTSyo8gNSNgk="} > $out/train-images-idx3-ubyte
    gunzip -c ${fetch "train-labels-idx1-ubyte" "sha256-NVJTSgpVi77WrtMrMMSVzKI9Vn7FLKyL4aBzDoAQJVw="} > $out/train-labels-idx1-ubyte
    gunzip -c ${fetch "t10k-images-idx3-ubyte" "sha256-jUIsewocHHkkWlvPB/6G4z7q/ueSuEWErsJ29aLbxOY="} > $out/t10k-images-idx3-ubyte
    gunzip -c ${fetch "t10k-labels-idx1-ubyte" "sha256-965g+S4A7G3r0jpgiMMdvSNx7KP/oN7677JZkkIErsY="} > $out/t10k-labels-idx1-ubyte
  '';
  # The store path reaches the Python code as a small module beside main.py.
  src = pkgs.runCommand "load-src" { } ''
    cp -r ${./src} $out
    chmod -R +w $out
    cat > $out/dataset.py <<EOF
    DIRECTORY = "${mnist}"
    SOURCE = "${mirror}/"
    EOF
  '';
in
{
  run = inputs.tsugumi.pythonCard {
    inherit sdk src;
    packages = ps: [ ps.numpy ps.matplotlib ];
  };
}
