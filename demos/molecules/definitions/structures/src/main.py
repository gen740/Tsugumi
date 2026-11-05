"""The structures as a grid, drawn by RDKit, labelled by matplotlib (which has Japanese glyphs)."""
import math

import matplotlib.pyplot as plt
from rdkit import Chem
from rdkit.Chem import Draw

from inputs.molecules.mol.Molecules import Molecules


def main(ctx):
    m = ctx.input("molecules", Molecules)
    n = m.NamesLength()
    cols = min(5, n)
    rows = math.ceil(n / cols)
    fig, axes = plt.subplots(rows, cols, figsize=(2.7 * cols, 2.6 * rows), squeeze=False)
    for ax in axes.ravel():
        ax.axis("off")
    for i in range(n):
        ax = axes.ravel()[i]
        ax.imshow(Draw.MolToImage(Chem.MolFromSmiles(m.Smiles(i).decode()), size=(420, 360)))
        ax.set_title(f"{m.Names(i).decode()}\n{m.Formula(i).decode()}", fontsize=10)
    fig.tight_layout()
    plt.show()
