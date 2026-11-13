# The documentation site (docs/site, Astro) on the working tree: `nix run .#dev-doc`
# serves it so that a saved document shows in the browser at once, `.#build-doc`
# writes build/docs, and `.#test-doc` checks that output. The Node modules come
# from the Nix store, built from docs/site/package-lock.json; nothing is
# installed with npm.
{
  lib,
  writeShellApplication,
  importNpmLock,
  nodejs,
  git,
}:
let
  nodeModules = importNpmLock.buildNodeModules {
    npmRoot = ../docs/site;
    inherit nodejs;
  };
  command =
    name: run:
    writeShellApplication {
      name = "tsugumi-${name}";
      runtimeInputs = [
        nodejs
        git
      ];
      text = ''
        root="$(git rev-parse --show-toplevel)"
        site="$root/docs/site"
        if [ ! -f "$site/astro.config.mjs" ]; then
          echo "${name}: run this inside the Tsugumi repository" >&2
          exit 1
        fi
        # Astro resolves its packages from the site's folder: link the store's
        # copy there, in place of a folder left by an earlier `npm ci`.
        if [ ! -L "$site/node_modules" ]; then rm -rf "$site/node_modules"; fi
        ln -sfn ${nodeModules}/node_modules "$site/node_modules"
        cd "$site"
        export ASTRO_TELEMETRY_DISABLED=1
        ${run}
      '';
    };
in
lib.mapAttrs command {
  dev-doc = ''exec node node_modules/astro/bin/astro.mjs dev --host 127.0.0.1 "$@"'';
  build-doc = ''exec node node_modules/astro/bin/astro.mjs build "$@"'';
  test-doc = ''exec node --test "$@" tests/*.test.mjs'';
}
