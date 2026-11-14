import os

import numpy as np


def parse(path):
    atoms = []
    for line in open(path):
        if line.startswith(("ATOM", "HETATM")):
            atoms.append({"rec": line[:6].strip(), "name": line[12:16].strip(), "resn": line[17:20].strip(),
                          "chain": line[21], "resi": int(line[22:26]),
                          "xyz": (float(line[30:38]), float(line[38:46]), float(line[46:54]))})
    return atoms


def main(inputs, report, run):
    p = inputs.params
    folder = inputs.data.value.decode()
    atoms = parse(os.path.join(folder, sorted(os.listdir(folder))[0]))
    lig_chain = p.ligandChain.decode()
    ligand = [a for a in atoms if a["chain"] == lig_chain and a["resn"] != "HOH"]
    protein = [a for a in atoms if a["rec"] == "ATOM" and a["chain"] != lig_chain]
    ca = [a for a in protein if a["name"] == "CA"]
    chains = sorted({a["chain"] for a in atoms if a["resn"] != "HOH"})
    for c in chains:
        res = {(a["resi"], a["resn"]) for a in atoms if a["chain"] == c and a["resn"] != "HOH"}
        report.log(f"chain {c}: {len(res)} residues, {sum(a['chain'] == c for a in atoms)} atoms"
              + (" (ligand)" if c == lig_chain else ""))
    np.savez(os.path.join(run.output_directory, "structure.npz"),
             protein_xyz=np.array([a["xyz"] for a in protein]),
             protein_res=np.array([f"{a['resn']}{a['resi']}" for a in protein]),
             ligand_xyz=np.array([a["xyz"] for a in ligand]),
             ca_xyz=np.array([a["xyz"] for a in ca]),
             ca_res=np.array([f"{a['resn']}{a['resi']}" for a in ca]))
    pts = [a["xyz"] for a in ca] + [a["xyz"] for a in ligand]
    colors = [0x4A90D9] * len(ca) + [0xE5484D] * len(ligand)
    report.points([x for x, _, _ in pts], [y for _, y, _ in pts], [z for _, _, z in pts], colors)
    return {'atoms': {"value": run.output_directory}}
