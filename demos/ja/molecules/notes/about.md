## 分子の物性と Rule of Five

分子を文字列で表す SMILES から、RDKit が物性を計算します。分子量は原子量の和です。

$$M = \sum_i n_i\, A_i$$

**Lipinski の Rule of Five** は、経口で吸収されやすい分子の経験則で、次のうち 2 つ以上に違反すると吸収されにくいとされます。

$$M \le 500,\quad \log P \le 5,\quad \mathrm{NH{+}OH} \le 5,\quad \mathrm{N{+}O} \le 10$$

**paste** の smiles に SMILES を貼って実行すると、その分子が全部の Card に加わります。読めない SMILES は、落ちずに理由を表示します。

logP は原子の寄与の和（Crippen）による推定で、測定値ではありません。経験則には例外があり、薬としての良し悪しを決めるものではありません。