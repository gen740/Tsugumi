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

  rubyFace =
    dir:
    pkgs.runCommand "tsugumi-card-sdk-ruby" { nativeBuildInputs = [ pkgs.python3 ]; } ''
      mkdir -p $out
      for f in ${lib.concatStringsSep " " schemas}; do
        ${flatc} -b --schema --bfbs-builtins -o $out ${dir}/$f.fbs
      done
      python3 ${sdkRoot}/codegen/card_ruby.py ${dir}/_contract.json $out
    '';

  languagesKnown = [ "cpp" "python" "ruby" ];

  # Whether this Card is built for another system than the one evaluating it
  # (x86_64-linux in the aarch64 VM): its binaries run under the emulator the
  # kernel registered (binfmt_misc).
  foreign = pkgs.stdenv.hostPlatform.system != builtins.currentSystem;

  # tsugumi-worker for this nixpkgs and system, for a package Card of another
  # system: the worker of tsugumid is built for the machine's own system and
  # cannot load the Card's main.so. Built from this nixpkgs, so the worker and
  # the Card share one glibc, libstdc++ and libpython; one build per nixpkgs
  # and system, reused by every such Card. The sources are those tsugumid was
  # built from (installed with it: share/tsugumi/sdk/worker).
  worker =
    let
      dir = sdkRoot + "/worker";
    in
    assert lib.assertMsg (builtins.pathExists dir)
      "a C++ or Python Card of another system needs the worker's sources, installed with tsugumid (share/tsugumi/sdk/worker); this SDK has none";
    pkgs.stdenv.mkDerivation {
      name = "tsugumi-worker";
      src = lib.fileset.toSource {
        root = sdkRoot;
        fileset = lib.fileset.unions [ dir (sdkRoot + "/include") (sdkRoot + "/src/worker_main.cc") ];
      };
      nativeBuildInputs = [ pkgs.flatbuffers ];
      buildInputs = [ pkgs.flatbuffers ];
      dontConfigure = true;
      # As tsugumid's CMake build generates them (CMakeLists.txt).
      buildPhase = ''
        mkdir -p gen/tsugumi/protocol
        flatc --cpp --scoped-enums --gen-mutable --keep-prefix --include-prefix tsugumi/protocol \
          -I worker/protocol -o gen/tsugumi/protocol worker/protocol/*.fbs
        # hosq is a system header there too: its warnings are not the worker's.
        $CXX -std=c++20 -O2 -Wall -Iinclude -Iworker/core -isystem worker/include -Igen \
          src/worker_main.cc worker/core/schema.cc worker/core/util/text.cc \
          -lflatbuffers -lpthread -ldl -o tsugumi-worker
      '';
      installPhase = ''
        install -Dm755 tsugumi-worker $out/bin/tsugumi-worker
      '';
    };
in
rec {
  inherit generated runtime worker;
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
      ruby = if want "ruby" then rubyFace dir else null;
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

  # The Python of Python Cards: nixpkgs' python3, except that the plotting
  # packages below are built without their test suites. They are often not in
  # the binary cache for the VM (aarch64-linux), and their tests pull large
  # builds (narwhals: duckdb; plotly: nodejs, bokeh, xarray): a Card using
  # plotly or altair took over half an hour to build the first time, against
  # a minute or two without the tests. scikit-learn's suite adds over ten minutes.
  cardPython =
    let
      noCheck = p: p.overridePythonAttrs { doCheck = false; };
      python = pkgs.python3.override {
        self = python;
        packageOverrides = _: super: {
          narwhals = noCheck super.narwhals;
          plotly = noCheck super.plotly;
          altair = noCheck super.altair;
          scikit-learn = noCheck super.scikit-learn;
        };
      };
    in
    python;

  # A Python Card: `src/main.py` defines `main(ctx)`. The package contains the
  # interpreter, the requested packages and a main.so that embeds Python.
  # `fonts`: font packages matplotlib falls back to for glyphs its own fonts
  # lack (by default a Japanese font when matplotlib is among the packages).
  pythonCard =
    {
      src,
      sdk,
      name ? "tsugumi-python-card",
      python ? cardPython,
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

  # Ruby uses MRI in the existing package ABI. All gems, including native
  # extensions, are built by bundlerEnv from Gemfile.lock and gemset.nix;
  # nothing installs packages when a Card runs.
  rubyCard =
    {
      src,
      sdk,
      name ? "tsugumi-ruby-card",
      ruby ? pkgs.ruby,
      gemdir ? null,
      gemConfig ? { },
      buildInputs ? [ ],
    }:
    assert lib.assertMsg (sdk.ruby != null) "rubyCard: the SDK needs ruby in languages";
    assert lib.assertMsg (
      gemdir == null
      || (
        builtins.pathExists (gemdir + "/Gemfile")
        && builtins.pathExists (gemdir + "/Gemfile.lock")
        && builtins.pathExists (gemdir + "/gemset.nix")
      )
    ) "rubyCard: gemdir needs Gemfile, Gemfile.lock and gemset.nix (generated by bundix)";
    let
      gems =
        if gemdir == null then
          null
        else
          pkgs.bundlerEnv {
            name = "${name}-gems";
            inherit ruby gemdir;
            gemConfig = pkgs.defaultGemConfig // gemConfig;
          };
      gemPath = if gems == null then "${ruby}/${ruby.gemPath}" else "${gems}/${ruby.gemPath}";
    in
    pkgs.stdenv.mkDerivation {
      inherit name src;
      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [
        ruby
        pkgs.flatbuffers
      ]
      ++ buildInputs;
      dontConfigure = true;
      buildPhase = ''
        runHook preBuild
        ruby_config=${ruby}/bin/ruby
        libruby=$($ruby_config -rrbconfig -e 'print File.join(RbConfig::CONFIG.fetch("libdir"), RbConfig::CONFIG.fetch("LIBRUBY_SO"))')
        $CC -O2 -fPIC -c ${sdkRoot}/src/ruby_main.c -o $NIX_BUILD_TOP/ruby_main.o \
          -I${sdkRoot}/include $(pkg-config --cflags ruby-${ruby.version.majMin}) \
          -DTSUGUMI_LIBRUBY='"'"$libruby"'"' \
          -DTSUGUMI_RUBY_SDK='"${sdkRoot}/ruby"' \
          -DTSUGUMI_RUBY_CARD='"'$out'/lib/card"' \
          -DTSUGUMI_RUBY_GEMS='"${gemPath}"' \
          -DTSUGUMI_RUBY_GEMFILE='"${if gemdir == null then "" else "${gemdir}/Gemfile"}"'
        $CXX -std=c++20 -O2 -fPIC -c ${sdkRoot}/src/ruby_json.cc -o $NIX_BUILD_TOP/ruby_json.o \
          -I${sdkRoot}/include -I${sdk.ruby} -DTSUGUMI_RUBY_CONTRACT='"${sdk.ruby}"'
        $CXX -shared $NIX_BUILD_TOP/ruby_main.o $NIX_BUILD_TOP/ruby_json.o -o $NIX_BUILD_TOP/main.so \
          $(pkg-config --libs ruby-${ruby.version.majMin}) -L${ruby}/lib -lflatbuffers \
          -Wl,-rpath,${ruby}/lib
        ${lib.optionalString (gems != null) ''
          BUNDLE_GEMFILE=${gemdir}/Gemfile GEM_HOME=${gemPath} GEM_PATH=${gemPath} ${ruby}/bin/ruby ${sdkRoot}/ruby/check_lock.rb ${gemdir}/Gemfile.lock
        ''}
        runHook postBuild
      '';
      installPhase = ''
        mkdir -p $out/lib/card
        cp -r --no-preserve=mode ./. $out/lib/card/
        cp $NIX_BUILD_TOP/main.so $out/lib/main.so
      '';
    };

  # An R Card: `src/main.R` defines `main(ctx)` (sdk/r/tsugumi.R). R has no
  # FlatBuffers generator, so the Card is a process that exchanges the port
  # messages as JSON: `run` is a bash script and the result has no `sdk`.
  # `packages` picks from nixpkgs' rPackages. `fonts`: what plots draw with
  # (Latin, then Japanese); a Card has no system fonts in its sandbox.
  rCard =
    {
      src,
      packages ? (ps: [ ]),
      fonts ? [ pkgs.dejavu_fonts pkgs.ipaexfont ],
    }:
    let
      r = pkgs.rWrapper.override { packages = [ pkgs.rPackages.jsonlite ] ++ packages pkgs.rPackages; };
    in
    ''
      export FONTCONFIG_FILE=${pkgs.makeFontsConf { fontDirectories = fonts; }}
      exec ${r}/bin/Rscript --vanilla ${sdkRoot + "/r/tsugumi.R"} ${src}
    '';

  # A Julia Card: `src/main.jl` defines `main(ctx)` (sdk/julia/tsugumi.jl),
  # a process that exchanges JSON as rCard does. `packages` are names from the
  # General registry, fixed at build time by julia-bin.withPackages.
  juliaCard =
    { src, packages ? [ ] }:
    let
      # Linux takes it from the binary cache. A macOS host (development only:
      # the app runs Cards in the Linux VM) would run Julia's whole test suite
      # first, which takes over half an hour.
      base =
        if pkgs.stdenv.hostPlatform.isDarwin then
          pkgs.julia-bin.overrideAttrs { doInstallCheck = false; }
        else
          pkgs.julia-bin;
      julia = if packages == [ ] then base else base.withPackages packages;
    in
    ''
      exec ${julia}/bin/julia --startup-file=no --history-file=no ${sdkRoot + "/julia/tsugumi.jl"} ${src}
    '';

  # Serializes the Card attribute set for tsugumid. Store paths in the
  # strings keep their closures alive through string context.
  #   run      the body: a package (derivation; the worker loads its
  #            lib/main.so and calls it in memory; for another system,
  #            run.worker names the worker built for it) or a bash script (string;
  #            inputs as $JSON_<PORT> files and $FILE_<PORT> / $DIR_<PORT>
  #            paths, outputs as JSON written to $TSUGUMI_OUTPUT)
  #   preRun   bash before the body, every invocation (also with a resident
  #            worker); no Card inputs; its environment and `cd` carry over
  #            to the call, not to the loading of a package, which comes first
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
          // lib.optionalAttrs foreign { worker = "${worker}/bin/tsugumi-worker"; }
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
