## Cas A (Chandra)

Makes a three-colour image from X-ray event data of the supernova remnant Cassiopeia A. Red is 600–1650 eV, green 1650–2250 eV and blue 2250–7500 eV.

The observation is pinned with Nix in the `default.nix` of **load**. It is fetched from HEASARC once, on the first run, and the file in the store is used after that (no network while running).

The analysis tool CIAO is pinned the same way in the `default.nix` of **ciao**. Its source is pinned by hash and built once, on the first run (it takes a few minutes), and **bands** counts the photons with that tool's `dmcopy`.

Change obsid, file and hash of **load** to these values for a deep observation. You can also compare how the remnant changed over about twenty years.

- 114 (2000, 50 ks, 520 MB): `acisf00114N006_evt2.fits.gz`, `sha256-l7CCu8BibZmfu8QyVF5yC7OQjkrwbnkW5AvWlWcyK/Y=`
- 19606 (2019, 49 ks, 349 MB): `acisf19606N002_evt2.fits.gz`, `sha256-FPE5ByrHvbx0Cxk5wVu/N8kjZCrsiFy5snxV1Ypcvtc=`

Change the bands of **bands** or the stretch of **rgb** and rerun just that Card.

**spectrum** draws the energy spectrum of the same photons with matplotlib and shows which element's lines fall in which colour.