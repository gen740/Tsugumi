# The documentation site

Run commands from the repository root. This README describes site development and is not a published document.

Published Markdown comes from the user guides in `docs/` and the design documents in `docs/design/`. The repository's `README.md` is the entry point. Edit these sources, never a generated copy. The [user index](../README.md) and [design index](../design/README.md) supply navigation groups and descriptions.

The site is an Astro project in `docs/site/`, with its own pinned dependencies. Nix supplies Node.js and builds the site's packages from `docs/site/package-lock.json` ([nix/docs-site.nix](../../nix/docs-site.nix)); `docs/site/node_modules` is a link to them, and nothing is installed with npm.

```bash
nix run .#dev-doc
```

Astro serves the site at `http://localhost:4321` and reloads when documents, components or illustrations change. To build and check the static output:

```bash
nix run .#build-doc
nix run .#test-doc
```

- **Output:** `build/docs/` contains the HTML, linked Markdown downloads, `llms.txt` and static assets. It is ignored by Git and can be deleted and rebuilt. After one of the commands above has made the link, `npm --prefix docs/site run preview` in the dev shell serves that output locally.
- **Content:** `src/content.config.ts` renders the catalog from `src/lib/documents.mjs` as an Astro content collection. The catalog reads the tables in the two indexes. Repository guides remain linked on GitHub. Astro renders Markdown; the `.astro` components provide the common layout, navigation and home page. `src/lib/documents.mjs` defines the story illustrations and resolves repository links for the website and Markdown downloads.
- **Publication:** [.github/workflows/docs.yml](../../.github/workflows/docs.yml) builds and checks the site on pull requests and publishes `build/docs/` on pushes to `main`. Set the repository's Pages source to **GitHub Actions**. No generated pages are committed.
- **Illustrations:** the Canvas modules in `docs/site/js/` run only on pages that use them. Story figure positions are defined in `STORY` in `src/lib/documents.mjs`; a missing anchor phrase fails the build.
- **Demo player:** the site commands take `js/player.js`, `player.css` and `player-data/` from `nix build .#site-player`, the shared output of the Web package. Generated files are ignored by Git; recorded runs and images in `docs/site/player-data/assets/` remain committed. A manual build uses `node docs/site/player/build.mjs` before serving or copying static assets. Set `TSUGUMI_DEMOS_DIR` to the pinned demos tree and prepare `ui/web/node_modules` and `ui/web/shared/gen`. See [player/README.md](player/README.md) for recording and SVG rendering.
- **Without JavaScript:** document text and links remain readable. Animation respects reduced motion.
