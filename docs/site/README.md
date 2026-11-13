# The documentation site

Run commands from the repository root. This README describes site development and is not a published document.

The top-level Markdown files in `docs/` are the source documents. The repository's `README.md` is the entry point; `THIRD_PARTY_NOTICES.md` stays with the license. Edit these sources, never a generated copy. [docs/README.md](../README.md) supplies the navigation groups and descriptions.

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
- **Content:** `src/content.config.ts` renders the catalog from `src/lib/documents.mjs` as an Astro content collection. The catalog reads the top-level `docs/README.md`, not a recursive Markdown glob; tooling READMEs stay outside it. Astro renders Markdown; the `.astro` components provide the common layout, navigation and home page. `src/lib/documents.mjs` defines the story illustrations and resolves repository links for the website and Markdown downloads.
- **Publication:** [.github/workflows/docs.yml](../../.github/workflows/docs.yml) builds and checks the site on pull requests and publishes `build/docs/` on pushes to `main`. Set the repository's Pages source to **GitHub Actions**. No generated pages are committed.
- **App help:** `macOS/build-app.sh` builds the same site and `macOS/tools/bundle-docs.sh` bundles its story pages and assets. The development app reads `build/docs/` when no bundled help is present. Relative URLs let the pages work under both GitHub Pages and the app's local URL scheme.
- **Illustrations:** the Canvas modules in `docs/site/js/` run only on pages that use them. Story figure positions are defined in `STORY` in `src/lib/documents.mjs`; a missing anchor phrase fails the build.
- **Demo player:** `docs/site/player/` builds the interactive gallery from the Web UI. Its shared outputs (`docs/site/js/player.js`, `docs/site/player.css`, `docs/site/player-data/`) are also used by the Web UI and native thumbnails. Regenerate them with `node docs/site/player/build.mjs` after changing the player inputs; `node docs/site/player/build.mjs --check` verifies them. This needs `web/node_modules` and `web/src/gen`.
- **Without JavaScript:** document text and links remain readable. Animation respects reduced motion.
