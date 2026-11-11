# Nix references browser check

Build with `node web/scripts/nix-reference-browser.mjs /tmp/tsugumi-nix-ui`, serve that directory on localhost, and open `index.html`. This uses the real canvas, reference controls, and Card settings against deterministic RPC fixtures; it is not a daemon end-to-end test.

Verified in the in-app browser on 2026-11-12:

- Toggle Nix references: two purple dashed arrows point from referencing to referenced Cards.
- Move the pointer onto source: source, analysis, and their link are highlighted; the unrelated link dims.
- Click a link: its row is selected in the reference list. The list navigates to a selected Card on the canvas and opens the referencing Card's Nix definition.
- Card settings → Packages: 205 recorded packages are paged 100/100/5; search on page 3 resets to page 1 and returns the matching package.
- Remove reference: the link and list entry disappear immediately.

Core RPC tests separately cover parsed references, invalid references, nested targets, deduplication, and saved-source updates. Mac tests cover nested target projection and camera transforms; the Mac app compiles with the same public RPC.
