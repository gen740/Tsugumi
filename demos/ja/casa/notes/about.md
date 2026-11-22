## Cas A（Chandra）

超新星残骸カシオペヤ座 A の X 線イベントデータから 3 色画像を作ります。赤は 600–1650 eV、緑は 1650–2250 eV、青は 2250–7500 eV です。

観測は **load** の `default.nix` で Nix に固定しています。最初の実行で HEASARC から一度だけ取得し、以後はストアのファイルを使います（実行中にネットワークは使いません）。

解析ツールの CIAO は **ciao** の `default.nix` で同じように固定しています。ソースを hash で固定して最初の実行で一度だけビルドし（数分かかります）、**bands** はそのツールの `dmcopy` で光子を数えます。

**load** の obsid・file・hash を次の値に変えると、深い観測になります。約 20 年の間に残骸がどう変わったかも比べられます。

- 114（2000 年、50 ks、520 MB）: `acisf00114N006_evt2.fits.gz`、`sha256-l7CCu8BibZmfu8QyVF5yC7OQjkrwbnkW5AvWlWcyK/Y=`
- 19606（2019 年、49 ks、349 MB）: `acisf19606N002_evt2.fits.gz`、`sha256-FPE5ByrHvbx0Cxk5wVu/N8kjZCrsiFy5snxV1Ypcvtc=`

**bands** の帯域や **rgb** の伸長を変えて、その Card だけを実行し直せます。

**spectrum** は同じ光子のエネルギースペクトルを matplotlib で描き、どの元素の輝線がどの色に入るかを示します。