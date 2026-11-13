# `nix run .#dev-doc`: the documentation site's dev server (Astro) on the working
# tree, so a saved document shows in the browser at once. The Node modules come
# from the Nix store, built from docs/site/package-lock.json; nothing is
# installed with npm.
{
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
in
writeShellApplication {
  name = "tsugumi-dev-doc";
  runtimeInputs = [
    nodejs
    git
  ];
  text = ''
    root="$(git rev-parse --show-toplevel)"
    site="$root/docs/site"
    if [ ! -f "$site/astro.config.mjs" ]; then
      echo "dev-doc: run this inside the Tsugumi repository" >&2
      exit 1
    fi
    # Astro resolves its packages from the site's folder. Link the store's copy
    # there, and leave a folder someone installed with npm as it is.
    if [ -L "$site/node_modules" ] || [ ! -e "$site/node_modules" ]; then
      ln -sfn ${nodeModules}/node_modules "$site/node_modules"
    fi
    cd "$site"
    export ASTRO_TELEMETRY_DISABLED=1
    exec node node_modules/astro/bin/astro.mjs dev --host 127.0.0.1 "$@"
  '';
}
