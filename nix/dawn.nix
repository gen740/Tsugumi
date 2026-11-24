{
  lib,
  stdenvNoCC,
  fetchurl,
}:
stdenvNoCC.mkDerivation {
  pname = "dawn-webgpu";
  version = "20261120.121236";

  # Dawn is not packaged in nixpkgs. Use Google's CI build of this commit.
  src = fetchurl {
    url = "https://github.com/google/dawn/releases/download/v20261120.121236/Dawn-fec851bab7b360449e3de9ebf771bd13d7942194-macos-latest-Release.tar.gz";
    sha256 = "f0ce00696b0d508cb8b9a075c308a5c99ad591feadc4e7d9f9b4a8560b4c04e0";
  };
  licenseFile = fetchurl {
    url = "https://raw.githubusercontent.com/google/dawn/fec851bab7b360449e3de9ebf771bd13d7942194/LICENSE";
    sha256 = "1c43dfd13b306e5bac47839ea90cd1882249a75c92963ade400d4c1165e42228";
  };

  dontBuild = true;
  dontStrip = true;
  installPhase = ''
    runHook preInstall
    mkdir -p "$out/lib" "$out/share/licenses/dawn"
    cp -R include "$out/"
    cp lib/libwebgpu_dawn.a "$out/lib/"
    cp "$licenseFile" "$out/share/licenses/dawn/LICENSE"
    runHook postInstall
  '';

  meta = {
    description = "Google Dawn's native WebGPU library for Apple silicon";
    homepage = "https://dawn.googlesource.com/dawn";
    license = lib.licenses.bsd3;
    platforms = [ "aarch64-darwin" ];
  };
}
