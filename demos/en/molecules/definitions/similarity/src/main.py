"""Tanimoto similarity of Morgan fingerprints for every pair of molecules."""
import matplotlib.pyplot as plt
import numpy as np
from rdkit import Chem, DataStructs
from rdkit.Chem import rdFingerprintGenerator


def main(inputs, report):
    m = inputs.molecules
    p = inputs.params
    n = len(m.names)
    names = [m.names[i].decode() for i in range(n)]
    generator = rdFingerprintGenerator.GetMorganGenerator(radius=p.radius, fpSize=p.bits)
    prints = [generator.GetFingerprint(Chem.MolFromSmiles(m.smiles[i].decode())) for i in range(n)]
    sim = np.array([DataStructs.BulkTanimotoSimilarity(f, prints) for f in prints])
    off = sim - np.eye(n)
    a, b = np.unravel_index(np.argmax(off), off.shape)
    mean = off.sum() / max(n * (n - 1), 1)
    report.log(f"mean similarity {mean:.4f}; most similar {names[a]} and {names[b]} {sim[a, b]:.4f}")

    fig, ax = plt.subplots(figsize=(8, 6.6))
    image = ax.imshow(sim, cmap="Blues", vmin=0, vmax=1)
    ax.set_xticks(range(n), names, rotation=45, ha="right")
    ax.set_yticks(range(n), names)
    for i in range(n):
        for j in range(n):
            ax.text(j, i, f"{sim[i, j]:.2f}"[1:] if sim[i, j] < 1 else "1", ha="center", va="center", fontsize=8,
                    color="white" if sim[i, j] > 0.55 else "#333333")
    ax.set_title(f"Tanimoto coefficient (Morgan radius {p.radius}): the closest are {names[a]} and {names[b]} ({sim[a, b]:.2f})",
                 fontsize=11)
    fig.colorbar(image, ax=ax, shrink=0.8)
    fig.tight_layout()
    report.figure()
