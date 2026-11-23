from card import Artifact, Input, Output, params as card_params
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def png(fig):
    import io
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=110, bbox_inches="tight")
    plt.close(fig)
    return buf.getvalue()


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    p = card_params
    d = np.load(os.path.join(inputs.require("atoms").open(), "structure.npz"))
    prot, res, lig = d["protein_xyz"], d["protein_res"], d["ligand_xyz"]
    dist = np.linalg.norm(prot[:, None, :] - lig[None, :, :], axis=2)
    near = dist.min(axis=1) <= p.cutoff
    rows = {}
    for r, dmin, n in zip(res[near], dist.min(axis=1)[near], (dist <= p.cutoff).sum(axis=1)[near]):
        old = rows.get(r, (99.0, 0))
        rows[r] = (min(old[0], dmin), old[1] + int(n))
    table = sorted(rows.items(), key=lambda x: x[1][0])
    ca, ca_res = d["ca_xyz"], d["ca_res"]
    m = np.linalg.norm(ca[:, None, :] - ca[None, :, :], axis=2)
    fig, ax = plt.subplots(figsize=(5.5, 5))
    ax.imshow(m < p.contact_angstrom, cmap="Greys", origin="lower")
    idx = [i for i, r in enumerate(ca_res) if r in rows]
    ax.scatter(idx, idx, s=12, c="#E5484D", label=f"pocket residues ({len(idx)})")
    ax.set_title(f"Ca contact map (< {p.contact_angstrom:g} A)", fontsize=10)
    ax.legend(loc="lower right", fontsize=8)
    with open(os.path.join(directory, "contact-map.png"), "wb") as fh:
        fh.write(png(fig))
    with open(os.path.join(directory, "pocket.csv"), "w") as fh:
        fh.write("residue,min_distance_A,contacts\n" + "".join(f"{r},{v[0]:.2f},{v[1]}\n" for r, v in table))
    print(f"{len(table)} residues within {p.cutoff:g} A of the ligand")
    # TODO(open): Port the demo display.
    return Output(pocket=artifact)
