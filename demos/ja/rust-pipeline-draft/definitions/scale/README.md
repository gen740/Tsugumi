---
description: Rust で受け取った値を掛ける
---

Rust で number.value に uint64 パラメータ factor（既定値 3）を掛け、結果を表示して out に出力する。固定した Cargo 雛形は demos/ja/rust-pipeline/definitions/scale を参照する。型は namespace t; table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;

入力の意図:
- number: Integer value

出力の意図:
- out: Integer value
