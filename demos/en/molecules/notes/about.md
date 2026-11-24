## Molecular properties and the Rule of Five

From SMILES, which writes a molecule as a string, RDKit computes properties. The molecular weight is the sum of the atomic weights.

$$M = \sum_i n_i\, A_i$$

**Lipinski's Rule of Five** is a rule of thumb for molecules readily absorbed when taken orally: violating two or more of the following is said to make absorption poor.

$$M \le 500,\quad \log P \le 5,\quad \mathrm{NH{+}OH} \le 5,\quad \mathrm{N{+}O} \le 10$$

Paste a SMILES into smiles of **properties** and run, and that molecule joins every Card. A SMILES that cannot be read shows the reason instead of crashing.

logP is an estimate by summing atomic contributions (Crippen), not a measurement. A rule of thumb has exceptions, and it does not decide whether a drug is good.