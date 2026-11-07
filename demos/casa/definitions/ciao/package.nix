# CIAO's command line tools from the source package of the CXC's conda channel.
# A plain function of pkgs, so that it can also be used without the flake.
{
  pkgs,
  ciaoSrc ? {
    version = "4.18.0";
    url = "https://cxc.cfa.harvard.edu/conda/ciao/noarch/ciao-src-4.18.0-h42e8d52_2948.conda";
    sha256 = "sha256-+x2qWmTFg+OcARtKfrDD/49V+exDNKE4ex/s4cmtsJU=";
  },
}:
let
  inherit (pkgs) lib stdenv fetchurl;

  # A .conda file is a zip holding a zstd tarball. The smoke test data (219 MB)
  # is left out; CMake goes on without it.
  source = pkgs.runCommand "ciao-src-${ciaoSrc.version}" {
    nativeBuildInputs = [
      pkgs.unzip
      pkgs.zstd
      pkgs.gnutar
    ];
  } ''
    unzip -q ${fetchurl { inherit (ciaoSrc) url sha256; }} 'pkg-*'
    zstd -dc pkg-*.tar.zst | tar -x --exclude='ciao-src/test_data/smoke' ciao-src
    mv ciao-src $out
  '';

  # Not in nixpkgs. CIAO looks for the static library.
  ccfits = stdenv.mkDerivation {
    pname = "ccfits";
    version = "2.6";
    src = fetchurl {
      url = "https://heasarc.gsfc.nasa.gov/FTP/software/fitsio/ccfits/v2.6/CCfits-2.6.tar.gz";
      sha256 = "2bb439db67e537d0671166ad4d522290859e8e56c2f495c76faa97bc91b28612";
    };
    nativeBuildInputs = [ pkgs.updateAutotoolsGnuConfigScriptsHook ];
    buildInputs = [ pkgs.cfitsio ];
    configureFlags = [
      "--with-cfitsio=${lib.getDev pkgs.cfitsio}"
      "--with-pic"
      "--enable-static"
    ];
    dontDisableStatic = true;
    enableParallelBuilding = true;
  };

  # CIAO links some tools to a static, threaded cfitsio as well as the shared one.
  cfitsioStatic = pkgs.cfitsio.overrideAttrs {
    outputs = [ "out" ];
    cmakeFlags = [
      "-DUSE_PTHREADS=ON"
      "-DUSE_BZIP2=OFF"
      "-DBUILD_SHARED_LIBS=OFF"
      "-DTESTS=OFF"
      "-DUTILS=OFF"
    ];
    doCheck = false;
    doInstallCheck = false;
    postInstall = "";
    postFixup = "";
  };

  # libwcs, which the channel packages as wcssubs (the wcssubs 3.9.5 tarball is
  # no longer on the server, so this takes it from wcstools).
  wcssubs = stdenv.mkDerivation {
    pname = "wcssubs";
    version = "3.9.7";
    src = fetchurl {
      url = "http://tdc-www.harvard.edu/software/wcstools/wcstools-3.9.7.tar.gz";
      sha256 = "525f6970eb818f822db75c1526b3122b1af078affa572dce303de37df5c7b088";
    };
    buildPhase = ''
      make -C libwcs CFLAGS="-fPIC -g -O2 -std=gnu89 -w"
    '';
    installPhase = ''
      install -Dm644 libwcs/libwcs.a $out/lib/libwcs.a
      install -d $out/include
      install -m644 libwcs/*.h $out/include/
    '';
  };
in
stdenv.mkDerivation {
  pname = "ciao";
  inherit (ciaoSrc) version;
  src = source;

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    bison
    flex
    perl
    gitMinimal # test_data/CMakeLists.txt requires it even when it fetches nothing
    meson
    (python312.withPackages (ps: [
      ps.pip
      ps.meson-python
      ps.setuptools
      ps.numpy
    ]))
  ];
  buildInputs = with pkgs; [
    wcssubs
    boost186 # the nearest to CIAO's 1.85; from 1.88 boost::process is a different API
    cfitsio
    ccfits
    fftw
    fftwFloat
    gsl
    gtest
    readline
    libjpeg
    xercesc
    zlib
    curl
    ncurses
  ];

  # ds9 is for looking at images by hand; CMake only records where it is.
  ASCDS_IMAGER_PATH = "/nonexistent";
  # Old code passes strings as printf formats.
  hardeningDisable = [ "format" ];
  # CIAO turns off fused multiply-add on macOS ARM so that its numbers do not
  # depend on the architecture, and has no build of its own for Linux ARM.
  env = lib.optionalAttrs (stdenv.hostPlatform.isAarch64 && stdenv.hostPlatform.isLinux) {
    NIX_CFLAGS_COMPILE = "-ffp-contract=off";
  };

  postPatch = ''
    # The Python modules (crates and the like) are not packaged yet.
    sed -i '/add_subdirectory(pyciao)/d' ciaolibs/CMakeLists.txt
  '';
  cmakeFlags = [
    # obsvis is the ds9 integration.
    "-DSEGMENTS=corelibs;ciaolibs;ciaotools"
    "-DCCFITS_SEARCH_PREFIX=${ccfits}"
    "-DCFITSIO_THREADED_LIBRARY=${cfitsioStatic}/lib/libcfitsio.a"
    "-DCMAKE_BUILD_TYPE=Release"
  ];

  setupHook = ./ciao-setup-hook.sh;

  meta = {
    description = "CIAO, the Chandra X-ray Center's analysis tools";
    homepage = "https://cxc.cfa.harvard.edu/ciao/";
    license = lib.licenses.gpl3Plus;
    platforms = [
      "aarch64-linux"
      "x86_64-linux"
      "aarch64-darwin"
    ];
  };
}
