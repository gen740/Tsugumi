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
      ${flatc} --python --gen-object-api -o $out ${dir}/$f.fbs
    done
    # For code that learns the port types at run time (tsugumi.contract).
    cp ${dir}/_contract.json $out/_contract.json
  '';

  goFace =
    dir:
    pkgs.runCommand "tsugumi-card-sdk-go" { nativeBuildInputs = [ pkgs.python3 ]; } ''
      mkdir -p $out/flatbuffers
      cp ${sdkRoot}/go/go.mod $out/go.mod
      cp ${sdkRoot}/go/runtime.go ${sdkRoot}/go/host.h $out/
      cp ${sdkRoot}/include/tsugumi/sdk/abi.h $out/abi.h
      cp ${sdkRoot}/src/go_host.c $out/host.c
      cp ${pkgs.flatbuffers.src}/go/*.go $out/flatbuffers/
      rm -f $out/flatbuffers/*_test.go
      for f in ${lib.concatStringsSep " " schemas}; do
        ${flatc} --go --gen-object-api --go-import tsugumi/flatbuffers --go-module-name tsugumi -o $out ${dir}/$f.fbs
      done
      python3 ${sdkRoot}/codegen/card_go.py ${dir}/_contract.json $out
    '';

  csharpFace = dir: pkgs.runCommand "tsugumi-card-sdk-csharp" { nativeBuildInputs = [ pkgs.python3 ]; } ''
    mkdir -p $out
    for f in ${lib.concatStringsSep " " schemas}; do
      ${flatc} --csharp --gen-object-api --cs-global-alias -o $out ${dir}/$f.fbs
    done
    python3 ${sdkRoot}/codegen/card_csharp.py ${dir}/_contract.json $out/CardBindings.cs ${sdkRoot}/include/tsugumi/sdk/abi.h
  '';

  languagesKnown = [ "cpp" "python" "go" "csharp" ];

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
      "a package Card of another system needs the worker's sources, installed with tsugumid (share/tsugumi/sdk/worker); this SDK has none";
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
      go = if want "go" then goFace dir else null;
      csharp = if want "csharp" then csharpFace dir else null;
    };

  csharpCard = import ./csharp.nix { inherit pkgs sdkRoot foreign; };

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

  # Go's runtime stays in the worker process until the process exits. Remote
  # modules use the Card's explicit go.sum and hash of its module cache.
  goCard =
    {
      src,
      sdk,
      name ? "tsugumi-go-card",
      lock ? null,
      hash ? null,
      buildInputs ? [ ],
      nativeBuildInputs ? [ ],
    }:
    assert lib.assertMsg (sdk.go != null) "goCard: the SDK was made without \"go\" in languages";
    assert lib.assertMsg ((lock == null) == (hash == null))
      "goCard: give `lock = ./src/go.sum;` and `hash` together for external modules";
    let
      # Invalidate the module cache when either Go dependency file changes.
      cacheKey = lib.optionalString (lock != null) ("-" + builtins.substring 0 16 (
        builtins.hashString "sha256" (builtins.hashFile "sha256" lock + builtins.hashFile "sha256" (src + "/go.mod"))
      ));
      checkLock = lib.optionalString (lock != null) ''
        if ! cmp -s go.sum ${lock}; then
          echo "goCard: go.sum is stale. Regenerate the Card's lock and update hash." >&2
          exit 1
        fi
      '';
    in
    pkgs.buildGoModule {
      inherit src buildInputs nativeBuildInputs;
      name = name + cacheKey;
      vendorHash = hash;
      proxyVendor = true;
      env.CGO_ENABLED = 1;
      modPostBuild = checkLock;
      postPatch = ''
        if [ -e tsugumi ] || [ -e tsugumi_adapter.go ]; then
          echo "src/tsugumi and src/tsugumi_adapter.go are reserved for the generated SDK" >&2
          exit 1
        fi
        ${lib.optionalString (lock != null) "install -m 0644 ${lock} go.sum"}
        cp -r --no-preserve=mode ${sdk.go} tsugumi
        cp tsugumi/adapter.go.in tsugumi_adapter.go
      '';
      buildPhase = ''
        runHook preBuild
        go build -mod=readonly -buildmode=c-shared -ldflags="-buildid=" -o main.so .
        ${checkLock}
        runHook postBuild
      '';
      checkPhase = ''
        runHook preCheck
        go test -mod=readonly ./...
        ${checkLock}
        runHook postCheck
      '';
      installPhase = ''
        runHook preInstall
        mkdir -p $out/lib
        cp main.so $out/lib/main.so
        runHook postInstall
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

  # The PyPI packages a uv lock names, for one interpreter and system.
  #
  # `wheels` is a fixed-output derivation: uv turns the lock into the exact
  # list of distributions with their hashes, and pip downloads the wheels that
  # fit this interpreter. Its name carries the hash of the lock and of
  # pyproject.toml, so an edited lock is fetched again and a `hash` that was
  # not updated with it is a hash mismatch, never the previous set of wheels.
  # The second derivation installs those files without the network.
  #
  # Wheels only: a source distribution would be compiled inside the
  # fixed-output derivation, and its bytes would differ from build to build.
  uvPackages =
    { python, lock, pyproject, hash, libraries }:
    let
      system = pkgs.stdenv.hostPlatform.system;
      outputHash =
        if builtins.isAttrs hash then
          hash.${system} or (throw "pythonCard: `hash` has no entry for ${system} (it names ${toString (builtins.attrNames hash)})")
        else
          hash;
      key = builtins.substring 0 16 (builtins.hashString "sha256"
        "${builtins.hashFile "sha256" lock}${builtins.hashFile "sha256" pyproject}${python.pythonVersion}${system}");
      wheels = pkgs.stdenvNoCC.mkDerivation {
        name = "tsugumi-python-wheels-${key}";
        dontUnpack = true;
        nativeBuildInputs = [ pkgs.uv (python.withPackages (ps: [ ps.pip ])) pkgs.cacert ];
        impureEnvVars = lib.fetchers.proxyImpureEnvVars;
        buildPhase = ''
          export HOME=$TMPDIR UV_NO_CACHE=1 UV_PYTHON_DOWNLOADS=never PIP_DISABLE_PIP_VERSION_CHECK=1
          export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
          mkdir project && cd project
          cp ${pyproject} pyproject.toml
          cp ${lock} uv.lock
          # --locked: a lock that no longer matches pyproject.toml stops here.
          if ! uv export --locked --no-dev --no-emit-project --format requirements-txt \
              --python ${python}/bin/python -o requirements.txt; then
            echo "pythonCard: uv.lock does not match pyproject.toml. Run \`uv lock\` in the Card's folder, then update \`hash\`." >&2
            exit 1
          fi
          mkdir -p $out
          if ! python -m pip download --no-deps --require-hashes --only-binary :all: \
              --no-cache-dir -r requirements.txt -d $out; then
            echo "pythonCard: a locked package has no wheel for Python ${python.pythonVersion} on ${system}. Only wheels are supported: take it from nixpkgs with \`packages\` instead." >&2
            exit 1
          fi
        '';
        dontInstall = true;
        dontFixup = true;
        outputHashMode = "recursive";
        outputHashAlgo = "sha256";
        inherit outputHash;
      };
    in
    pkgs.stdenv.mkDerivation {
      name = "tsugumi-python-locked-${key}";
      dontUnpack = true;
      nativeBuildInputs = [ (python.withPackages (ps: [ ps.pip ])) ]
        ++ lib.optional pkgs.stdenv.hostPlatform.isLinux pkgs.autoPatchelfHook;
      # What compiled wheels expect from the system (manylinux), and what the Card adds.
      buildInputs = lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.stdenv.cc.cc.lib pkgs.zlib ] ++ libraries;
      installPhase = ''
        mkdir -p $out
        shopt -s nullglob
        files=(${wheels}/*.whl)
        if [ ''${#files[@]} -gt 0 ]; then
          python -m pip install --no-index --no-deps --no-compile --no-cache-dir \
            --disable-pip-version-check --target $out "''${files[@]}"
        fi
        rm -rf $out/bin
      '';
      passthru = { inherit wheels; };
    };

  # A Python Card: `src/main.py` defines `main(ctx)`. The package contains the
  # interpreter, the requested packages and a main.so that embeds Python.
  # `packages` picks from the pinned nixpkgs. `lock` (the Card's own uv.lock,
  # with pyproject.toml beside it) and `hash` together add exactly the PyPI
  # packages the lock names; nothing is resolved when the Card is built, so a
  # Card without `lock` has no PyPI package. `hash` is the hash of the
  # downloaded wheels: one string, or one per system
  # (`{ aarch64-linux = "sha256-..."; }`). `libraries`: shared libraries the
  # locked wheels load beyond the C and C++ runtimes and zlib.
  # `fonts`: font packages matplotlib falls back to for glyphs its own fonts
  # lack (by default a Japanese font when matplotlib is among the packages).
  pythonCard =
    {
      src,
      sdk,
      name ? "tsugumi-python-card",
      python ? cardPython,
      packages ? (ps: [ ]),
      lock ? null,
      hash ? null,
      pyproject ? (if lock == null then null else dirOf lock + "/pyproject.toml"),
      libraries ? [ ],
      fonts ? null,
    }:
    assert lib.assertMsg (sdk.python != null) "pythonCard: the SDK was made without \"python\" in languages";
    assert lib.assertMsg (lock == null || hash != null)
      "pythonCard: `lock` needs `hash`, the hash of the wheels it names. Write `hash = pkgs.lib.fakeHash;`, run the Card once and copy the hash the failure reports as `got:`";
    assert lib.assertMsg (hash == null || lock != null)
      "pythonCard: `hash` is given without `lock`. Name the Card's lock: `lock = ./uv.lock;`";
    assert lib.assertMsg (lock == null || builtins.pathExists pyproject)
      "pythonCard: no pyproject.toml beside the lock. uv.lock is made from it: keep both in the Card's folder";
    let
      env = python.withPackages (ps: [ ps.flatbuffers ] ++ packages ps);
      locked = lib.optionalString (lock != null)
        "${uvPackages { inherit python lock pyproject hash libraries; }}:";
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
          -DTSUGUMI_PY_PATH='"'$out'/lib/python:${locked}${env}/${python.sitePackages}"'
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

  # R and Julia Cards embed one interpreter in the resident ABI worker.
  # Their list/Dict API uses the worker's in-memory FlatBuffers JSON adapter.
  rCard =
    {
      src,
      name ? "tsugumi-r-card",
      packages ? (ps: [ ]),
      fonts ? [
        pkgs.dejavu_fonts
        pkgs.ipaexfont
      ],
    }:
    pkgs.stdenv.mkDerivation {
      inherit name src;
      buildInputs = [
        pkgs.R
        pkgs.rPackages.jsonlite
      ]
      ++ pkgs.rWrapper.recommendedPackages
      ++ packages pkgs.rPackages;
      dontConfigure = true;
      buildPhase = ''
        runHook preBuild
        $CC -O2 -fPIC -shared -Wall -Wextra -Werror ${sdkRoot}/src/r_main.c -o $NIX_BUILD_TOP/main.so \
          -I${sdkRoot}/include -I${pkgs.R}/lib/R/include -L${pkgs.R}/lib/R/lib -lR \
          -Wl,-rpath,${pkgs.R}/lib/R/lib \
          -DTSUGUMI_LIBR='"${pkgs.R}/lib/R/lib/libR${pkgs.stdenv.hostPlatform.extensions.sharedLibrary}"' \
          -DTSUGUMI_R_HOME='"${pkgs.R}/lib/R"' -DTSUGUMI_R_LIBS='"'"$R_LIBS_SITE"'"' \
          -DTSUGUMI_R_FONTS='"${pkgs.makeFontsConf { fontDirectories = fonts; }}"' \
          -DTSUGUMI_R_SDK='"'$out'/lib/tsugumi.R"' -DTSUGUMI_R_SOURCE='"'$out'/lib/card"'
        runHook postBuild
      '';
      installPhase = ''
        mkdir -p $out/lib/card
        cp -r --no-preserve=mode ./. $out/lib/card/
        cp ${sdkRoot}/r/tsugumi.R $out/lib/tsugumi.R
        cp $NIX_BUILD_TOP/main.so $out/lib/main.so
      '';
    };

  # Packages are fixed by the same nixpkgs Julia wrapper as standalone Cards.
  juliaCard =
    {
      src,
      name ? "tsugumi-julia-card",
      packages ? [ ],
    }:
    let
      # macOS host development avoids Julia’s expensive upstream install tests.
      base =
        if pkgs.stdenv.hostPlatform.isDarwin then
          pkgs.julia-bin.overrideAttrs { doInstallCheck = false; }
        else
          pkgs.julia-bin;
      julia = if packages == [ ] then base else base.withPackages packages;
    in
    pkgs.stdenv.mkDerivation {
      inherit name src;
      dontConfigure = true;
      # Runtime headers are external; keep warnings fatal for our shim.
      buildPhase = ''
        runHook preBuild
        $CC -O2 -fPIC -shared -Wall -Wextra -Werror ${sdkRoot}/src/julia_main.c -o $NIX_BUILD_TOP/main.so \
          -I${sdkRoot}/include -isystem ${base}/include/julia -L${base}/lib -ljulia \
          -Wl,-rpath,${base}/lib \
          -DTSUGUMI_LIBJULIA='"${base}/lib/libjulia${pkgs.stdenv.hostPlatform.extensions.sharedLibrary}"' \
          -DTSUGUMI_JULIA_ENV='"'$out'/lib/julia-env"' -DTSUGUMI_SHIM='"'$out'/lib/main.so"' \
          -DTSUGUMI_JULIA_SDK='"'$out'/lib/tsugumi.jl"' -DTSUGUMI_JULIA_SOURCE='"'$out'/lib/card"'
        # Keep only wrapper settings, never a builder's HOME or temporary depot.
        JULIA_DEPOT_PATH= ${julia}/bin/julia --startup-file=no --history-file=no -e '
          for key in ["JULIA_DEPOT_PATH", "JULIA_PROJECT", "JULIA_LOAD_PATH", "LD_LIBRARY_PATH", "FONTCONFIG_FILE", "PYTHONHOME", "PYTHONPATH", "PYTHON", "JULIA_PYTHONCALL_EXE", "JULIA_CONDAPKG_OFFLINE", "JULIA_CONDAPKG_BACKEND"]
            haskey(ENV, key) && println(key, "=", ENV[key])
          end' > $NIX_BUILD_TOP/julia-env
        runHook postBuild
      '';
      installPhase = ''
        mkdir -p $out/lib/card
        cp -r --no-preserve=mode ./. $out/lib/card/
        cp ${sdkRoot}/julia/tsugumi.jl $out/lib/tsugumi.jl
        cp $NIX_BUILD_TOP/main.so $NIX_BUILD_TOP/julia-env $out/lib/
      '';
    };

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
