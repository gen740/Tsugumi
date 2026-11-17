# License metadata for runtime package names; absent metadata stays absent.
{ pkgs, wanted }:
let
  flatten =
    license:
    if builtins.isString license then
      [ license ]
    else if builtins.isList license then
      builtins.concatMap flatten license
    else if license ? licenses then
      flatten license.licenses
    else if license ? exception then
      [ "${license.license.spdxId} WITH ${license.exception.spdxId}" ]
    else
      pkgs.lib.optional (license ? spdxId) license.spdxId;
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
  };
in
builtins.listToAttrs (
  map (
    name:
    let
      package = pkgs.${aliases.${name} or name} or pkgs.nix.libs.${name} or { };
    in
    {
      inherit name;
      value = {
        licenses = flatten (package.meta.license or [ ]);
        outputs = package.outputs or [ ];
      };
    }
  ) wanted
)
