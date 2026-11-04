# Files of Help > Tsugumi を知る

`build-app.sh` copies these, with `README.md`, `docs/Philosophy.md` and `docs/Name.md`, to `Contents/Resources/explainer/`.

| File | Source | License |
| --- | --- | --- |
| `YuseiMagic-Regular.ttf` | [google/fonts `ofl/yuseimagic`](https://github.com/google/fonts/tree/f58dfb9001f31a5eb15b4960b9548b8fd0fa6168/ofl/yuseimagic) at `f58dfb9`, unmodified (sha256 `82098615f39ed9da6a8ccc674b9006e49c70dd5b775a7a1697f6bedd22ce25a2`) | SIL Open Font License 1.1: [`YuseiMagic-OFL.txt`](YuseiMagic-OFL.txt), bundled with it |
| `casa-rgb-25628.png` | The `rgb` Card's output in a run of the bundled Cas A · Chandra RGB demo (ObsID 25628; its band counts are the ones the figure's table shows) | Derived from Chandra observation data fetched from HEASARC; not Tsugumi-authored data (see [THIRD_PARTY_NOTICES.md](../../../THIRD_PARTY_NOTICES.md#external-data)) |

The font is registered for the app's own process (`CTFontManagerRegisterFontsForURL`, `.process`); nothing is installed on the system.
