# Tsugumi Card SDK for Nix (`inputs.tsugumi` in a Card's default.nix).
#
#   { pkgs, inputs, params }:
#   {
#     package = inputs.tsugumi.cppCard { src = ./src; };
#     reuseWorker = true;
#   }
#
# `generated` holds the core-generated _inputs.fbs, _outputs.fbs and
# _params.fbs of the Card being built.
{ pkgs, generated }:
let
  inherit (pkgs) lib;
  sdk = ./..;
  flatc = "${pkgs.flatbuffers}/bin/flatc";
  schemas = [ "_inputs" "_outputs" "_params" ];

  cppHeaders = pkgs.runCommand "tsugumi-card-cpp-headers" { } ''
    mkdir -p $out/include
    for f in ${lib.concatStringsSep " " schemas}; do
      ${flatc} --cpp --scoped-enums -o $out/include ${generated}/$f.fbs
    done
  '';

  pythonBindings = pkgs.runCommand "tsugumi-card-python-bindings" { } ''
    mkdir -p $out
    for f in ${lib.concatStringsSep " " schemas}; do
      ${flatc} --python -o $out ${generated}/$f.fbs
    done
  '';
in
rec {
  inherit generated;
  inherit (pkgs) flatbuffers;
  headers = cppHeaders;

  # A C++ Card: every *.cc below `src` is compiled into lib/main.so.
  cppCard =
    { src, name ? "tsugumi-cpp-card", buildInputs ? [ ], cxxflags ? [ ] }:
    pkgs.stdenv.mkDerivation {
      inherit name src;
      buildInputs = [ pkgs.flatbuffers ] ++ buildInputs;
      dontConfigure = true;
      buildPhase = ''
        runHook preBuild
        sources=$(find . -name '*.cc' | sort)
        if [ -z "$sources" ]; then echo "no .cc sources in src" >&2; exit 1; fi
        $CXX -std=c++20 -O2 -fPIC -shared -Wall \
          -I${cppHeaders}/include -I${sdk}/include ${lib.escapeShellArgs cxxflags} \
          $sources -o main.so
        runHook postBuild
      '';
      installPhase = ''
        mkdir -p $out/lib
        cp main.so $out/lib/main.so
      '';
    };

  # A Python Card: `src/main.py` defines `main(ctx)`. The package contains the
  # interpreter, the requested packages and a main.so that embeds Python.
  pythonCard =
    { src, name ? "tsugumi-python-card", python ? pkgs.python3, packages ? (ps: [ ]) }:
    let
      env = python.withPackages (ps: [ ps.flatbuffers ] ++ packages ps);
    in
    pkgs.stdenv.mkDerivation {
      inherit name src;
      dontConfigure = true;
      buildPhase = ''
        runHook preBuild
        $CC -O2 -fPIC -shared ${sdk}/src/python_main.c -o $NIX_BUILD_TOP/main.so \
          -I${sdk}/include -I${python}/include/python${python.pythonVersion} \
          -L${python}/lib -lpython${python.pythonVersion} \
          -Wl,-rpath,${python}/lib \
          -DTSUGUMI_PY_HOME='"${python}"' \
          -DTSUGUMI_LIBPYTHON='"${python}/lib/libpython${python.pythonVersion}${pkgs.stdenv.hostPlatform.extensions.sharedLibrary}"' \
          -DTSUGUMI_PY_PATH='"'$out'/lib/python:${env}/${python.sitePackages}"'
        runHook postBuild
      '';
      installPhase = ''
        mkdir -p $out/lib/python/card
        cp -r --no-preserve=mode ./. $out/lib/python/card/
        touch $out/lib/python/card/__init__.py
        cp -r --no-preserve=mode ${pythonBindings}/. $out/lib/python/
        cp -r --no-preserve=mode ${sdk}/python/tsugumi $out/lib/python/
        cp $NIX_BUILD_TOP/main.so $out/lib/main.so
      '';
    };

  # Serializes the Card attribute set for tsugumid. Store paths in the scripts
  # keep their closures alive through string context.
  manifest =
    card:
    let
      # `exports` is free-form: values, functions and derivations that
      # downstream Cards read through inputs.upstream."<path>".exports.
      known = [ "sdk" "package" "preScript" "script" "postScript" "reuseWorker" "exports" ];
      unknown = lib.subtractLists known (builtins.attrNames card);
    in
    assert lib.assertMsg (unknown == [ ]) "unknown Card attributes: ${toString unknown}";
    pkgs.writeText "tsugumi-card.json" (builtins.toJSON (
      { reuseWorker = card.reuseWorker or false; }
      // lib.optionalAttrs (card ? package) { package = "${card.package}"; }
      // lib.optionalAttrs (card ? preScript) { inherit (card) preScript; }
      // lib.optionalAttrs (card ? script) { inherit (card) script; }
      // lib.optionalAttrs (card ? postScript) { inherit (card) postScript; }
    ));
}
