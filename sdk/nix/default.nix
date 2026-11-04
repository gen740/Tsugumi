# Tsugumi Card SDK for Nix (`inputs.tsugumi` in a Card's default.nix).
#
#   { pkgs, inputs, params }:
#   let
#     sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
#   in {
#     run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };   # a package
#   }
#
#   { pkgs, inputs, params }: { run = ''
#     jq '{out: {value: .value}}' "$JSON_IN" > "$TSUGUMI_OUTPUT"
#   ''; }                                                            # bash
#
# `generated` holds the core-generated contract of the Card being built
# (_inputs.fbs, _outputs.fbs, _params.fbs and _contract.json).
{ pkgs, generated }:
let
  inherit (pkgs) lib;
  sdkRoot = ./..;
  flatc = "${pkgs.flatbuffers}/bin/flatc";
  schemas = [ "_inputs" "_outputs" "_params" ];

  # Only these files go into the shared runtime, so it does not change (and
  # is not rebuilt) when anything else in the SDK or any Card changes.
  runtimeSrc = lib.fileset.toSource {
    root = sdkRoot;
    fileset = lib.fileset.unions [ (sdkRoot + "/include") (sdkRoot + "/src/card_runtime.cc") ];
  };

  # libtsugumi_card.so: call glue, context, logging and tsugumi::visualization.
  # Independent of any Card's contract: one build per environment.
  runtime = pkgs.stdenv.mkDerivation {
    name = "tsugumi-card-runtime";
    src = runtimeSrc;
    buildInputs = [ pkgs.flatbuffers ];
    dontConfigure = true;
    # Cards find it through their rpath (Linux) or its absolute install name (macOS hosts).
    buildPhase = ''
      $CXX -std=c++20 -O2 -fPIC -shared -Wall -Iinclude src/card_runtime.cc -o libtsugumi_card.so \
        ${if pkgs.stdenv.hostPlatform.isDarwin then "-Wl,-install_name,$out/lib/libtsugumi_card.so" else "-Wl,-soname,libtsugumi_card.so"}
    '';
    installPhase = ''
      mkdir -p $out/lib $out/include
      cp libtsugumi_card.so $out/lib/
      cp -r include/. $out/include/
    '';
  };

  # The contract files under their fixed names (flatc names its output after them).
  contractDir = { inputs, outputs, params, contract }:
    pkgs.runCommand "tsugumi-card-contract" { } ''
      mkdir -p $out
      cp ${inputs} $out/_inputs.fbs
      cp ${outputs} $out/_outputs.fbs
      cp ${params} $out/_params.fbs
      cp ${contract} $out/_contract.json
    '';

  cppFace = dir: pkgs.runCommand "tsugumi-card-sdk-cpp" { nativeBuildInputs = [ pkgs.python3 ]; } ''
    mkdir -p $out/include $out/src
    for f in ${lib.concatStringsSep " " schemas}; do
      ${flatc} --cpp --gen-object-api --scoped-enums -o $out/include ${dir}/$f.fbs
    done
    python3 ${sdkRoot}/codegen/card_cpp.py ${dir}/_contract.json $out
  '';

  pythonFace = dir: pkgs.runCommand "tsugumi-card-sdk-python" { } ''
    mkdir -p $out
    for f in ${lib.concatStringsSep " " schemas}; do
      ${flatc} --python -o $out ${dir}/$f.fbs
    done
  '';

  languagesKnown = [ "cpp" "python" ];
in
rec {
  inherit generated runtime;
  inherit (pkgs) flatbuffers;

  # The SDK of this Card: its contract compiled for the requested languages
  # only (nothing is generated or built for languages left out).
  sdk =
    { languages
    , inputs ? "${generated}/_inputs.fbs"
    , outputs ? "${generated}/_outputs.fbs"
    , params ? "${generated}/_params.fbs"
    , contract ? "${generated}/_contract.json"
    }:
    let
      unknown = lib.subtractLists languagesKnown languages;
      dir = contractDir { inherit inputs outputs params contract; };
      want = l: builtins.elem l languages;
    in
    assert lib.assertMsg (languages != [ ]) "sdk: name the languages, e.g. languages = [ \"cpp\" ]";
    assert lib.assertMsg (unknown == [ ]) "sdk: unknown languages ${toString unknown} (known: ${toString languagesKnown})";
    {
      inherit languages;
      contract = dir;
      cpp = if want "cpp" then cppFace dir else null;
      python = if want "python" then pythonFace dir else null;
    };

  # A C++ Card: every *.cc below `src` (with `sdk`: plus the generated
  # adapter) compiled into lib/main.so, linked against the shared runtime.
  cppCard =
    { src, sdk, name ? "tsugumi-cpp-card", buildInputs ? [ ], cxxflags ? [ ] }:
    assert lib.assertMsg (sdk.cpp != null) "cppCard: the SDK was made without \"cpp\" in languages";
    pkgs.stdenv.mkDerivation {
      inherit name src;
      buildInputs = [ pkgs.flatbuffers ] ++ buildInputs;
      dontConfigure = true;
      buildPhase = ''
        runHook preBuild
        sources=$(find . -name '*.cc' | sort)
        if [ -z "$sources" ]; then echo "no .cc sources in src" >&2; exit 1; fi
        $CXX -std=c++20 -O2 -fPIC -shared -Wall \
          -I${sdk.cpp}/include -I${runtime}/include ${lib.escapeShellArgs cxxflags} \
          $sources ${sdk.cpp}/src/card_adapter.cc \
          -L${runtime}/lib -ltsugumi_card -Wl,-rpath,${runtime}/lib -o main.so
        runHook postBuild
      '';
      installPhase = ''
        mkdir -p $out/lib
        cp main.so $out/lib/main.so
      '';
    };

  # A Python Card: `src/main.py` defines `main(ctx)`. The package contains the
  # interpreter, the requested packages and a main.so that embeds Python.
  # `fonts`: font packages matplotlib falls back to for glyphs its own fonts
  # lack (by default a Japanese font when matplotlib is among the packages).
  pythonCard =
    {
      src,
      sdk,
      name ? "tsugumi-python-card",
      python ? pkgs.python3,
      packages ? (ps: [ ]),
      fonts ? null,
    }:
    assert lib.assertMsg (sdk.python != null) "pythonCard: the SDK was made without \"python\" in languages";
    let
      env = python.withPackages (ps: [ ps.flatbuffers ] ++ packages ps);
      bindings = sdk.python;
      usesMatplotlib = lib.any (p: (p.pname or "") == "matplotlib") (packages python.pkgs);
      fallbackFonts = if fonts != null then fonts else lib.optional usesMatplotlib pkgs.ipaexfont;
    in
    pkgs.stdenv.mkDerivation {
      inherit name src;
      dontConfigure = true;
      buildPhase = ''
        runHook preBuild
        $CC -O2 -fPIC -shared ${sdkRoot}/src/python_main.c -o $NIX_BUILD_TOP/main.so \
          -I${sdkRoot}/include -I${python}/include/python${python.pythonVersion} \
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
        cp -r --no-preserve=mode ${bindings}/. $out/lib/python/
        cp -r --no-preserve=mode ${sdkRoot}/python/tsugumi $out/lib/python/
        # The fallback fonts' files, one per line, for tsugumi.mpl (they stay in the closure).
        for f in ${lib.concatStringsSep " " (map toString fallbackFonts)}; do
          find "$f/share/fonts" -type f \( -name '*.ttf' -o -name '*.otf' \) | sort
        done > $out/lib/python/tsugumi/_fonts.txt
        cp $NIX_BUILD_TOP/main.so $out/lib/main.so
      '';
    };

  # Serializes the Card attribute set for tsugumid. Store paths in the
  # strings keep their closures alive through string context.
  #   run      the body: a package (derivation; the worker loads its
  #            lib/main.so and calls it in memory) or a bash script (string;
  #            inputs as $JSON_<PORT> files and $FILE_<PORT> / $DIR_<PORT>
  #            paths, outputs as JSON written to $TSUGUMI_OUTPUT)
  #   preRun   bash before the body, every invocation (also with a resident
  #            worker); no Card inputs; its environment and `cd` carry over
  #   postRun  bash at the end, also after a failed step (TSUGUMI_FAILED=1; it
  #            cannot turn a failure into success), not after a stop or a
  #            missed deadline (the invocation is ended at once)
  #   exports  free-form values, functions and derivations that downstream
  #            Cards read through inputs.upstream."<path>".exports
  # Whether the worker stays across invocations and runs (reuseWorker) is a
  # setting of the Card (_card.json), not of its build.
  manifest =
    card:
    let
      known = [ "sdk" "run" "preRun" "postRun" "exports" ];
      unknown = lib.subtractLists known (builtins.attrNames card);
      body =
        if !(card ? run) then throw "a Card needs `run`: a package (derivation) or a bash script (string)"
        else if lib.isDerivation card.run then { package = "${card.run}"; }
        else if builtins.isString card.run then { bash = card.run; }
        else throw "run must be a package (derivation) or a bash script (string)";
    in
    assert lib.assertMsg (unknown == [ ]) "unknown Card attributes: ${toString unknown} (known: ${toString known})";
    pkgs.writeText "tsugumi-card.json" (builtins.toJSON (
      { run = body; }
      // lib.optionalAttrs (card ? preRun) { inherit (card) preRun; }
      // lib.optionalAttrs (card ? postRun) { inherit (card) postRun; }
    ));
}
