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
      [ "${license.license.spdxId} WITH ${license.exception}" ]
    else
      [ (license.spdxId or license.shortName or license.fullName) ];
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
  direct = builtins.filter (item: item != null) (
    map (inspect pkgs) (builtins.filter (name: builtins.hasAttr name pkgs) attributeNames)
  );
  scopes = [
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
  missing = builtins.filter (path: !(builtins.elem path (map (item: item.path) narrow))) paths;
  resolved = narrow ++ lib.optionals (missing != [ ]) (scan pkgs);
in
builtins.listToAttrs (
  map (item: {
    name = item.path;
    value = item.licenses;
  }) resolved
)
