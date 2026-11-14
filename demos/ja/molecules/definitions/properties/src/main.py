"""Reads the molecules and computes their properties with RDKit. A SMILES that
cannot be read is reported with the reason and skipped."""
import tsugumi
import numpy as np
from rdkit import Chem, RDLogger
from rdkit.Chem import Crippen, Descriptors, Lipinski, rdMolDescriptors

from inputs.pasted.mol.Pasted import Pasted
from tsugumi import Failure

RDLogger.DisableLog("rdApp.*")  # reasons are shown on the Card instead


def read(smiles):
    """The molecule and None, or None and why it could not be read."""
    mol = Chem.MolFromSmiles(smiles, sanitize=False)
    if mol is None:
        return None, "SMILES の文法として読めません"
    problems = Chem.DetectChemistryProblems(mol)
    if problems:
        return None, f"化学として成り立ちません（RDKit: {problems[0].Message()}）"
    Chem.SanitizeMol(mol)
    return mol, None


def entries(inputs):
    """(name, SMILES) from the list, then the pasted one."""
    p = inputs.params
    pairs = []
    for i in range(len(p.molecules)):
        name, _, smiles = p.molecules[i].decode().partition(":")
        pairs.append((name.strip(), smiles.strip()) if smiles else (f"#{i + 1}", name.strip()))
    pasted = inputs.input("pasted", Pasted)
    smiles = (pasted.Smiles() or b"").decode().strip()
    if smiles:
        pairs.append(((pasted.Name() or b"pasted").decode().strip() or "pasted", smiles))
    return pairs


def strings(items):
    offsets = [s for s in items]
    _b_items = []
    for o in reversed(offsets):
        _b_items.append(o)
    return list(reversed(_b_items))


def main(inputs, report):
    rows, errors = [], []
    for name, smiles in entries(inputs):
        mol, why = read(smiles)
        if mol is None:
            errors.append(f"**{name}** の SMILES `{smiles}` は計算から外しました: {why}。")
            report.log(str(f"skipped {name}: {smiles}: {why}"))
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
    lines += [f"**{len(rows)} 分子の物性**（RDKit で計算）", "",
              "| 名前 | 分子式 | 分子量 | logP | TPSA (Å²) | HBD | HBA | 回転可能結合 |",
              "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    lines += [f"| {r['name']} | {r['formula']} | {r['mw']:.2f} | {r['logp']:.2f} | {r['tpsa']:.2f} | "
              f"{r['hbd']} | {r['hba']} | {r['rotatable']} |" for r in rows]
    report.markdown("\n".join(lines))
    (names, smiles, formula) = (strings([r[k] for r in rows]) for k in ('name', 'smiles', 'formula'))
    doubles = [np.array([r[k] for r in rows], dtype=np.float64) for k in ('mw', 'logp', 'tpsa')]
    ints = [np.array([r[k] for r in rows], dtype=np.int32) for k in ('hbd', 'hba', 'rotatable', 'nh_oh', 'n_o')]
    _out_value = {}
    _out_value['names'] = names
    _out_value['smiles'] = smiles
    _out_value['formula'] = formula
    _out_value['mw'] = doubles[0]
    _out_value['logp'] = doubles[1]
    _out_value['tpsa'] = doubles[2]
    _out_value['hbd'] = ints[0]
    _out_value['hba'] = ints[1]
    _out_value['rotatable'] = ints[2]
    _out_value['nh_oh'] = ints[3]
    _out_value['n_o'] = ints[4]
    return {'molecules': _out_value}
