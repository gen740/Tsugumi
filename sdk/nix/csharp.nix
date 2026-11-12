# C# uses the standard Card ABI and a process-owned .NET runtime. The SDK,
# FlatBuffers runtime sources and host headers come from the same pinned pkgs.
{ pkgs, sdkRoot }:
{
  src,
  sdk,
  name ? "tsugumi-csharp-card",
  packages ? [ ],
  nugetDeps ? null,
}:
let
  inherit (pkgs) lib;
  dotnet = pkgs.dotnet-sdk_10;
  runtime = dotnet.runtime;
  system = if pkgs.stdenv.hostPlatform.isDarwin then "osx" else "linux";
  arch = if pkgs.stdenv.hostPlatform.isAarch64 then "arm64" else "x64";
  rid = "${system}-${arch}";
  xml = text: builtins.replaceStrings [ "&" "\"" "<" ">" ] [ "&amp;" "&quot;" "&lt;" "&gt;" ] text;
  prepared = pkgs.runCommand "tsugumi-csharp-source" { } ''
    mkdir -p $out/Generated $out/FlatBuffers $out/Tsugumi
    cp -r ${src}/. $out/
    cp -r ${sdk.csharp}/. $out/Generated/
    cp ${pkgs.flatbuffers.src}/net/FlatBuffers/*.cs $out/FlatBuffers/
    cp ${sdkRoot}/csharp/*.cs $out/Tsugumi/
    cat > $out/Card.csproj <<'PROJECT'
    <Project Sdk="Microsoft.NET.Sdk">
      <PropertyGroup>
        <TargetFramework>net10.0</TargetFramework>
        <AssemblyName>Card</AssemblyName>
        <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
        <GenerateRuntimeConfigurationFiles>true</GenerateRuntimeConfigurationFiles>
        <TreatWarningsAsErrors>true</TreatWarningsAsErrors>
      </PropertyGroup>
      <ItemGroup>
        ${lib.concatMapStringsSep "\n" (
          p: ''<PackageReference Include="${xml p.name}" Version="${xml p.version}" />''
        ) packages}
      </ItemGroup>
    </Project>
    PROJECT
  '';
  managed = pkgs.buildDotnetModule {
    pname = name;
    version = "0";
    src = prepared;
    dotnet-sdk = dotnet;
    projectFile = "Card.csproj";
    inherit nugetDeps;
    executables = [ ];
  };
in
assert lib.assertMsg (sdk.csharp != null) "csharpCard requires sdk languages = [ \"csharp\" ]";
assert lib.assertMsg (
  packages == [ ] || nugetDeps != null
) "csharpCard packages require a fixed NuGet dependency file (nugetDeps)";
pkgs.stdenv.mkDerivation {
  inherit name;
  passthru = lib.optionalAttrs (nugetDeps != null) { fetch-deps = managed.fetch-deps; };
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
    cp -r ${managed}/lib/${name}/. $out/lib/managed/
  '';
}
