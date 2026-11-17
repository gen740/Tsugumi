# Resolve license declarations by actual derivation owner, including scoped
# libraries and aliases. Unrecognized outputs are never classified as our code.
{ pkgs, wanted }:
let
  inherit (pkgs) lib;
  names = map (item: item.name) wanted;
  paths = map (item: item.path) wanted;
  flatten =
    license:
    if builtins.isString license then
      [ license ]
    else if builtins.isList license then
      builtins.concatMap flatten license
    else if license ? licenses then
      flatten license.licenses
    else if license ? exception then
      [ "${identifier license.license} WITH ${identifier license.exception}" ]
    else
      [ (identifier license) ];
  identifier = license: license.spdxId or license.shortName or license.fullName;
  inspect =
    scope: attr:
    let
      attempt = builtins.tryEval (
        let
          package = scope.${attr};
        in
        if lib.isDerivation package && builtins.elem package.name names then
          let
            path = builtins.unsafeDiscardStringContext (baseNameOf package.drvPath);
          in
          if builtins.elem path paths then
            {
              inherit path;
              licenses = flatten (package.meta.license or [ ]);
            }
          else
            null
        else
          null
      );
    in
    if attempt.success then attempt.value else null;
  scan = scope: builtins.filter (item: item != null) (map (inspect scope) (builtins.attrNames scope));
  aliases = {
    bash-interactive = "bashInteractive";
    boehm-gc = "boehmgc";
    db = "db48";
    gcc = "gcc-unwrapped";
    gmp-with-cxx = "gmp";
    icu4c = "icu";
    libcap-ng = "libcap_ng";
    nss-cacert = "cacert";
    systemd-minimal-libs = "systemdMinimal";
    util-linux-minimal = "util-linux";
    xgcc = "gcc-unwrapped";
  };
  attributeNames = lib.unique (
    map (
      name:
      let
        pname = (builtins.parseDrvName name).name;
      in
      aliases.${pname} or pname
    ) names
  );
  attempt =
    value: default:
    let
      result = builtins.tryEval value;
    in
    if result.success then result.value else default;
  lookup =
    set: attrs:
    builtins.filter (item: item != null) (
      map (inspect set) (builtins.filter (name: builtins.hasAttr name set) attrs)
    );
  unresolved =
    found: builtins.filter (path: !(builtins.elem path (map (item: item.path) found))) paths;
  direct = lookup pkgs attributeNames;
  # Nix's own components (nix-cli, nix-nswrapper); removed versions throw.
  nixComponents = builtins.filter (scope: scope != null) (
    map (
      name: attempt (lib.seq (builtins.attrNames pkgs.nixVersions.${name}) pkgs.nixVersions.${name}) null
    ) (builtins.filter (lib.hasPrefix "nixComponents_") (builtins.attrNames pkgs.nixVersions))
  );
  scopes =
    nixComponents
    ++ [
      pkgs.nix.libs
      pkgs.llvmPackages
      pkgs.xorg
      {
        inherit (pkgs.stdenv.cc) cc;
        libc = pkgs.stdenv.cc.libc;
      }
    ]
    ++ lib.optional (builtins.any (name: lib.hasPrefix "python" name) names) pkgs.python3Packages;
  narrow = direct ++ builtins.concatMap scan scopes;

  # Owners no attribute names: bootstrap builds (the compiler glibc links,
  # stdenv's gmp) and a package's private override of a dependency (Nix's
  # boehm-gc). They are reached from stdenv and the packages found so far
  # through the inputs their own derivations name.
  edges = [
    "buildInputs"
    "nativeBuildInputs"
    "propagatedBuildInputs"
    "propagatedNativeBuildInputs"
    "depsBuildBuild"
    "depsHostHost"
    "depsTargetTarget"
    "stdenv"
    "cc"
    "libc"
    "bintools"
    "initialPath"
    "extraBuildInputs"
    "extraNativeBuildInputs"
  ];
  node = package: {
    key = attempt package.drvPath "";
    inherit package;
  };
  inputsOf =
    package:
    builtins.filter (input: attempt (lib.isDerivation input) false) (
      builtins.concatMap (edge: attempt (lib.toList (package.${edge} or [ ])) [ ]) edges
    );
  missingNarrow = unresolved narrow;
  reachable = builtins.genericClosure {
    startSet = map node (
      [ pkgs.stdenv ]
      ++ map (item: pkgs.${item}) (builtins.filter (name: builtins.hasAttr name pkgs) attributeNames)
    );
    operator = item: builtins.filter (next: next.key != "") (map node (inputsOf item.package));
  };
  reached = lib.optionals (missingNarrow != [ ]) (
    builtins.filter (item: item != null) (
      map (
        item:
        let
          path = builtins.unsafeDiscardStringContext (baseNameOf item.key);
        in
        if builtins.elem path missingNarrow then
          {
            inherit path;
            licenses = flatten (item.package.meta.license or [ ]);
          }
        else
          null
      ) reachable
    )
  );
  walked = narrow ++ reached;

  # What glibc names only in its build script (libidn2): the same attribute
  # in stdenv's bootstrap stages. Only names still unresolved are looked up,
  # as some attributes of the early stages cannot be evaluated at all. The
  # first stage is a raw set of arguments, not a package set.
  missingWalked = unresolved walked;
  stages = set: lib.optionals (!(set.__raw or false)) ([ set ] ++ stages set.stdenv.__bootPackages);
  pname = name: (builtins.parseDrvName name).name;
  unresolvedNames = lib.unique (
    map (item: aliases.${pname item.name} or (pname item.name)) (
      builtins.filter (item: builtins.elem item.path missingWalked) wanted
    )
  );
  boot = lib.optionals (missingWalked != [ ]) (
    builtins.concatMap (stage: lookup stage unresolvedNames) (builtins.tail (stages pkgs))
  );
  staged = walked ++ boot;
  resolved = staged ++ lib.optionals (unresolved staged != [ ]) (scan pkgs);
in
builtins.listToAttrs (
  map (item: {
    name = item.path;
    value = item.licenses;
  }) resolved
)
