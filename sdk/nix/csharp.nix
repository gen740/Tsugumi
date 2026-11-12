# C# uses the standard Card ABI and a process-owned .NET runtime. The SDK,
# FlatBuffers runtime sources and host headers come from the same pinned pkgs.
#
# External NuGet packages are used only when the Card names its own lock:
#
#   run = inputs.tsugumi.csharpCard {
#     inherit sdk; src = ./src;
#     lock = ./packages.lock.json;   # NuGet's own lock file, kept in the Card
#     hash = "sha256-...";           # NAR hash of the .nupkg files the lock names
#   };
#
# The lock is the only list of packages: its Direct entries become the
# project's package references, and every package it resolves to is fetched
# into one fixed-output derivation, `<name>-nuget-lock-<sha256 of the lock>`,
# whose NAR hash (sha256, recursive) is `hash`. Restore then runs without
# network in NuGet's locked mode, from that folder only, so NuGet also checks
# every package against the lock's contentHash. Without `lock` the Card has no
# external packages. Nothing is locked for you: a new lock starts with
# `hash = pkgs.lib.fakeHash`, and the Card's build log shows the hash to keep.
{ pkgs, sdkRoot, foreign }:
{
  src,
  sdk,
  name ? "tsugumi-csharp-card",
  # The .NET SDK the Card is compiled with; its runtime is the one hosted in
  # the worker. Any nixpkgs dotnet-sdk_N with a `.runtime` works.
  dotnet ? pkgs.dotnet-sdk_10,
  lock ? null,
  hash ? null,
}:
let
  inherit (pkgs) lib;
  runtime = dotnet.runtime;
  framework = "net${lib.versions.majorMinor runtime.version}";
  system = if pkgs.stdenv.hostPlatform.isDarwin then "osx" else "linux";
  arch = if pkgs.stdenv.hostPlatform.isAarch64 then "arm64" else "x64";
  rid = "${system}-${arch}";
  xml = text: builtins.replaceStrings [ "&" "\"" "<" ">" ] [ "&amp;" "&quot;" "&lt;" "&gt;" ] text;

  sections = if lock == null then { } else (builtins.fromJSON (builtins.readFile lock)).dependencies or { };
  entries = section: lib.mapAttrsToList (id: p: p // { inherit id; }) section;
  # What the project asks for, exactly as the lock recorded it.
  direct = builtins.filter (p: p.type == "Direct") (entries (sections.${framework} or { }));
  # Every package the lock resolves to, in any of its sections, by the
  # lowercase id and version nuget.org publishes it under.
  resolved = lib.unique (
    map (p: { id = lib.toLower p.id; version = lib.toLower p.resolved; })
      (builtins.filter (p: p.type != "Project") (lib.concatMap entries (builtins.attrValues sections)))
  );
  file = p: "${p.id}.${p.version}.nupkg";
  plain = s: builtins.match "[a-z0-9][a-z0-9._+-]*" s != null;
  unsafe = builtins.filter (p: !(plain p.id && plain p.version)) resolved;

  # The name carries the lock's own hash: a changed lock is a different
  # derivation, so an old `hash` is checked against the new lock's packages
  # instead of matching a store path fetched for the old one.
  packagesName = "${name}-nuget-lock-${builtins.substring 0 16 (builtins.hashFile "sha256" lock)}";
  # The .nupkg files of the lock, as published, side by side (a NuGet local
  # feed). `hash` is the NAR hash of this folder.
  nupkgs = pkgs.runCommand packagesName
    {
      nativeBuildInputs = [ pkgs.curl ];
      SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
      impureEnvVars = lib.fetchers.proxyImpureEnvVars;
      outputHashMode = "recursive";
      outputHash = hash; # SRI, "sha256-..."
    }
    ''
      mkdir -p $out
      ${lib.concatMapStrings (p: ''
        curl --fail --silent --show-error --location --retry 3 \
          -o $out/${file p} https://api.nuget.org/v3-flatcontainer/${p.id}/${p.version}/${file p}
      '') resolved}
    '';

  # The only package source is the lock's folder. Given with --configfile, it
  # is the only NuGet setting read: no user, machine, source-tree or fallback
  # folder settings.
  nugetConfig = pkgs.writeText "${name}-NuGet.Config" ''
    <?xml version="1.0" encoding="utf-8"?>
    <configuration>
      <packageSources>
        <clear />
        ${lib.optionalString (lock != null) ''<add key="lock" value="${nupkgs}" />''}
      </packageSources>
      <fallbackPackageFolders>
        <clear />
      </fallbackPackageFolders>
    </configuration>
  '';

  prepared = pkgs.runCommand "tsugumi-csharp-source" { } ''
    mkdir -p $out/Generated $out/FlatBuffers $out/Tsugumi
    cp -r ${src}/. $out/
    cp -r ${sdk.csharp}/. $out/Generated/
    cp ${pkgs.flatbuffers.src}/net/FlatBuffers/*.cs $out/FlatBuffers/
    cp ${sdkRoot}/csharp/*.cs $out/Tsugumi/
    ${lib.optionalString (lock != null) "cp ${lock} $out/packages.lock.json"}
    cat > $out/Card.csproj <<'PROJECT'
    <Project Sdk="Microsoft.NET.Sdk">
      <PropertyGroup>
        <TargetFramework>${framework}</TargetFramework>
        <AssemblyName>Card</AssemblyName>
        <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
        <GenerateRuntimeConfigurationFiles>true</GenerateRuntimeConfigurationFiles>
        <TreatWarningsAsErrors>true</TreatWarningsAsErrors>
        <NuGetAudit>false</NuGetAudit>
      </PropertyGroup>
      <ItemGroup>
        ${lib.concatMapStringsSep "\n" (
          p: ''<PackageReference Include="${xml p.id}" Version="${xml p.requested}" />''
        ) direct}
      </ItemGroup>
    </Project>
    PROJECT
  '';
  # Card.dll and the assemblies of its packages. The build has no network;
  # restore reads only nugetConfig above.
  managed = pkgs.stdenv.mkDerivation {
    name = "${name}-managed";
    src = prepared;
    nativeBuildInputs = [ dotnet ];
    DOTNET_CLI_TELEMETRY_OPTOUT = "1";
    DOTNET_NOLOGO = "1";
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1";
    DOTNET_GENERATE_ASPNET_CERTIFICATE = "false";
    # Integrity is the fixed-output hash and the lock's contentHash; an
    # offline build cannot check certificate revocation.
    DOTNET_NUGET_SIGNATURE_VERIFICATION = "false";
    configurePhase = ''
      runHook preConfigure
      export HOME=$TMPDIR/home DOTNET_CLI_HOME=$TMPDIR/home
      export NUGET_PACKAGES=$TMPDIR/packages NUGET_HTTP_CACHE_PATH=$TMPDIR/http-cache
      unset NUGET_FALLBACK_PACKAGES
      dotnet restore Card.csproj --configfile ${nugetConfig} ${lib.optionalString (lock != null) "--locked-mode"}
      runHook postConfigure
    '';
    buildPhase = ''
      runHook preBuild
      dotnet publish Card.csproj --no-restore --configuration Release \
        -p:Deterministic=true -p:ContinuousIntegrationBuild=true --output $out
      runHook postBuild
    '';
    dontInstall = true;
    dontFixup = true;
  };
  hashTarget = "the NAR hash (sha256) of the .nupkg files that lock names, built as ${packagesName}";
in
assert lib.assertMsg (sdk.csharp != null) "csharpCard requires sdk languages = [ \"csharp\" ]";
assert lib.assertMsg (!foreign)
  "csharpCard: a C# Card cannot be built for another system (${pkgs.stdenv.hostPlatform.system}); the .NET runtime is not run under emulation. Remove the Card's system setting";
assert lib.assertMsg (lock != null || hash == null)
  "csharpCard: hash is the NAR hash of the NuGet packages a lock names; give lock = ./packages.lock.json as well";
assert lib.assertMsg (lock == null || hash != null)
  "csharpCard: lock needs hash, ${hashTarget}. Set hash = pkgs.lib.fakeHash, run the Card, and copy the hash after `got:` in the Card's build log";
assert lib.assertMsg (lock == null || sections ? ${framework})
  "csharpCard: ${toString lock} has no ${framework} section; make the lock with this Card's .NET (${framework})";
assert lib.assertMsg (unsafe == [ ])
  "csharpCard: ${toString lock} names packages that are not plain NuGet ids and versions: ${toString (map file unsafe)}";
pkgs.stdenv.mkDerivation {
  inherit name;
  # The folder `hash` covers, for `nix build` / `nix hash path` by hand.
  passthru = lib.optionalAttrs (lock != null) { nugetPackages = nupkgs; };
  dontUnpack = true;
  dontConfigure = true;
  buildPhase = ''
    $CXX -std=c++20 -O2 -fPIC -shared -Wall -Wextra -Werror \
      -I${sdkRoot}/include \
      -I${dotnet}/share/dotnet/packs/Microsoft.NETCore.App.Host.${rid}/${runtime.version}/runtimes/${rid}/native \
      -DTSUGUMI_HOSTFXR='"${runtime}/share/dotnet/host/fxr/${runtime.version}/libhostfxr${pkgs.stdenv.hostPlatform.extensions.sharedLibrary}"' \
      -DTSUGUMI_DOTNET_ROOT='"${runtime}/share/dotnet"' \
      -DTSUGUMI_ASSEMBLY='"'$out'/lib/managed/Card.dll"' \
      -DTSUGUMI_RUNTIMECONFIG='"'$out'/lib/managed/Card.runtimeconfig.json"' \
      ${sdkRoot}/csharp/host.cc -o main.so ${lib.optionalString pkgs.stdenv.hostPlatform.isLinux "-ldl"}
  '';
  installPhase = ''
    mkdir -p $out/lib/managed
    cp main.so $out/lib/
    cp -r ${managed}/. $out/lib/managed/
  '';
}
