import tsugumi
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from inputs.data.protein.PdbFiles import PdbFiles
import outputs.atoms.protein.AtomsDir as out


def emit(inputs, port, out):
    result_ports = {}
    s = '.'
    _out_value = {}
    _out_value['value'] = s
    result_ports.update({port: _out_value})
    return result_ports


def png(fig):
    import io
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=110, bbox_inches="tight")
    plt.close(fig)
    return buf.getvalue()


def parse(path):
    atoms = []
    for line in open(path):
        if line.startswith(("ATOM", "HETATM")):
            atoms.append({"rec": line[:6].strip(), "name": line[12:16].strip(), "resn": line[17:20].strip(),
                          "chain": line[21], "resi": int(line[22:26]),
                          "xyz": (float(line[30:38]), float(line[38:46]), float(line[46:54]))})
    return atoms


def main(inputs, report, run):
    result_ports = {}
    p = inputs.params
    folder = inputs.input("data", PdbFiles).Value().decode()
    atoms = parse(os.path.join(folder, sorted(os.listdir(folder))[0]))
    lig_chain = p.ligandChain.decode()
    ligand = [a for a in atoms if a["chain"] == lig_chain and a["resn"] != "HOH"]
    protein = [a for a in atoms if a["rec"] == "ATOM" and a["chain"] != lig_chain]
    ca = [a for a in protein if a["name"] == "CA"]
    chains = sorted({a["chain"] for a in atoms if a["resn"] != "HOH"})
    for c in chains:
        res = {(a["resi"], a["resn"]) for a in atoms if a["chain"] == c and a["resn"] != "HOH"}
        report.log(str(f"chain {c}: {len(res)} residues, {sum(a['chain'] == c for a in atoms)} atoms"
              + (" (ligand)" if c == lig_chain else "")))
    np.savez(os.path.join(run.output_directory, "structure.npz"),
             protein_xyz=np.array([a["xyz"] for a in protein]),
             protein_res=np.array([f"{a['resn']}{a['resi']}" for a in protein]),
             ligand_xyz=np.array([a["xyz"] for a in ligand]),
             ca_xyz=np.array([a["xyz"] for a in ca]),
             ca_res=np.array([f"{a['resn']}{a['resi']}" for a in ca]))
    pts = [a["xyz"] for a in ca] + [a["xyz"] for a in ligand]
    colors = [0x4A90D9] * len(ca) + [0xE5484D] * len(ligand)
    report.points([x for x, _, _ in pts], [y for _, y, _ in pts], [z for _, _, z in pts], colors)
    result_ports.update(emit(inputs, 'atoms', out))
    return result_ports
