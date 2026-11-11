"""Reads the molecules and computes their properties with RDKit. A SMILES that
cannot be read is reported with the reason and skipped."""
import numpy as np
from rdkit import Chem, RDLogger
from rdkit.Chem import Crippen, Descriptors, Lipinski, rdMolDescriptors

from inputs.pasted.mol.Pasted import Pasted
from tsugumi import Failure
import outputs.molecules.mol.Molecules as out

RDLogger.DisableLog("rdApp.*")  # reasons are shown on the Card instead


def read(smiles):
    """The molecule and None, or None and why it could not be read."""
    mol = Chem.MolFromSmiles(smiles, sanitize=False)
    if mol is None:
        return None, "it cannot be parsed as SMILES"
    problems = Chem.DetectChemistryProblems(mol)
    if problems:
        return None, f"it does not make chemical sense (RDKit: {problems[0].Message()})"
    Chem.SanitizeMol(mol)
    return mol, None


def entries(ctx):
    """(name, SMILES) from the list, then the pasted one."""
    p = ctx.params()
    pairs = []
    for i in range(p.MoleculesLength()):
        name, _, smiles = p.Molecules(i).decode().partition(":")
        pairs.append((name.strip(), smiles.strip()) if smiles else (f"#{i + 1}", name.strip()))
    pasted = ctx.input("pasted", Pasted)
    smiles = (pasted.Smiles() or b"").decode().strip()
    if smiles:
        pairs.append(((pasted.Name() or b"pasted").decode().strip() or "pasted", smiles))
    return pairs


def strings(b, items):
    offsets = [b.CreateString(s) for s in items]
    b.StartVector(4, len(offsets), 4)
    for o in reversed(offsets):
        b.PrependUOffsetTRelative(o)
    return b.EndVector()


def main(ctx):
    rows, errors = [], []
    for name, smiles in entries(ctx):
        mol, why = read(smiles)
        if mol is None:
            errors.append(f"The SMILES `{smiles}` of **{name}** was left out of the computation: {why}.")
            print(f"skipped {name}: {smiles}: {why}")
            continue
        rows.append(dict(
            name=name, smiles=smiles, formula=rdMolDescriptors.CalcMolFormula(mol),
            mw=Descriptors.MolWt(mol), logp=Crippen.MolLogP(mol), tpsa=rdMolDescriptors.CalcTPSA(mol),
            hbd=rdMolDescriptors.CalcNumHBD(mol), hba=rdMolDescriptors.CalcNumHBA(mol),
            rotatable=rdMolDescriptors.CalcNumRotatableBonds(mol),
            nh_oh=Lipinski.NHOHCount(mol), n_o=Lipinski.NOCount(mol),
        ))
    if not rows:
        raise Failure("output_invalid", "no readable molecule: " + " ".join(errors))

    lines = [*errors, ""] if errors else []
    lines += [f"**Properties of {len(rows)} molecules** (computed with RDKit)", "",
              "| Name | Formula | Molecular weight | logP | TPSA (Å²) | HBD | HBA | Rotatable bonds |",
              "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    lines += [f"| {r['name']} | {r['formula']} | {r['mw']:.2f} | {r['logp']:.2f} | {r['tpsa']:.2f} | "
              f"{r['hbd']} | {r['hba']} | {r['rotatable']} |" for r in rows]
    ctx.display_markdown("\n".join(lines))

    b = ctx.builder(8192)
    names, smiles, formula = (strings(b, [r[k] for r in rows]) for k in ("name", "smiles", "formula"))
    doubles = [b.CreateNumpyVector(np.array([r[k] for r in rows], dtype=np.float64)) for k in ("mw", "logp", "tpsa")]
    ints = [b.CreateNumpyVector(np.array([r[k] for r in rows], dtype=np.int32))
            for k in ("hbd", "hba", "rotatable", "nh_oh", "n_o")]
    out.Start(b)
    out.AddNames(b, names)
    out.AddSmiles(b, smiles)
    out.AddFormula(b, formula)
    out.AddMw(b, doubles[0])
    out.AddLogp(b, doubles[1])
    out.AddTpsa(b, doubles[2])
    out.AddHbd(b, ints[0])
    out.AddHba(b, ints[1])
    out.AddRotatable(b, ints[2])
    out.AddNhOh(b, ints[3])
    out.AddNO(b, ints[4])
    ctx.output("molecules", b, out.End(b))
