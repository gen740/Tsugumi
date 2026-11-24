---
title: ciao
description: 解析ツール CIAO を Nix で固定する
---

CIAO のソースは `tsugumi.lock` でコミットとハッシュを固定します。Card はプロジェクトの nixpkgs を渡して `<ciao/src/package.nix>` を import します。
