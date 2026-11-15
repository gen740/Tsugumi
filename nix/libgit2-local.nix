# libgit2 for the Mac app's host service (`tsugumi-hostd`), which only reads
# a project's repository on the local disk: a static library with no network
# transport, so the app bundle carries no dylib for it and nothing from the
# Nix store. zlib, the regex engine (PCRE) and the HTTP parser (llhttp, which
# libgit2 cannot be built without) are the copies in libgit2's own source.
#
# `tsugumid` keeps nixpkgs' libgit2: it loads Nix's fetchers, which use that
# shared library with its transports, and one process must not hold two.
{ libgit2 }:
(libgit2.override {
  staticBuild = true;
  zlib = null;
  libssh2 = null;
  openssl = null;
  pcre2 = null;
  llhttp = null;
  libiconv = null;
}).overrideAttrs
  (old: {
    pname = "libgit2-local";
    # Later flags win over the ones of nixpkgs (pcre2, llhttp, SSH).
    cmakeFlags = old.cmakeFlags ++ [
      "-DUSE_SSH=OFF"
      "-DUSE_HTTPS=OFF"
      "-DUSE_SHA1=CollisionDetection"
      "-DUSE_SHA256=builtin"
      "-DUSE_NTLMCLIENT=OFF"
      "-DUSE_GSSAPI=OFF"
      "-DUSE_ICONV=OFF"
      "-DUSE_BUNDLED_ZLIB=ON"
      "-DREGEX_BACKEND=builtin"
      "-DUSE_HTTP_PARSER=builtin"
      "-DBUILD_CLI=OFF"
      "-DBUILD_TESTS=OFF"
    ];
    buildInputs = [ ];
    propagatedBuildInputs = [ ];
    # The tests are not built.
    doCheck = false;
    passthru = { };
    # No `git2` command is built.
    meta = removeAttrs old.meta [ "mainProgram" ] // {
      description = "libgit2 as a static library for reading local repositories";
    };
  })
