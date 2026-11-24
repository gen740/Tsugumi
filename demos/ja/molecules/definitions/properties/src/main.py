"""Reads the molecules and computes their properties with RDKit. A SMILES that
cannot be read is reported with the reason and skipped."""

from card import Input, Output, params
from mol.Molecules import MoleculesT

import numpy as np
from rdkit import Chem, RDLogger
from rdkit.Chem import Crippen, Descriptors, Lipinski, rdMolDescriptors

from tsugumi import Failure, Markdown, send

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


def entries():
    """(name, SMILES) from the list, then the pasted one."""
    p = params
    pairs = []
    for i in range(len(p.molecules)):
        name, _, smiles = p.molecules[i].partition(":")
        pairs.append((name.strip(), smiles.strip()) if smiles else (f"#{i + 1}", name.strip()))
    smiles = p.smiles.strip()
    if smiles:
        pairs.append((p.name.strip() or "pasted", smiles))
    return pairs


def run(_input: Input) -> Output:
    rows, errors = [], []
    for name, smiles in entries():
        mol, why = read(smiles)
        if mol is None:
            errors.append(f"**{name}** の SMILES `{smiles}` は計算から外しました: {why}。")
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
    lines += [f"**{len(rows)} 分子の物性**（RDKit で計算）", "",
              "| 名前 | 分子式 | 分子量 | logP | TPSA (Å²) | HBD | HBA | 回転可能結合 |",
              "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    lines += [f"| {r['name']} | {r['formula']} | {r['mw']:.2f} | {r['logp']:.2f} | {r['tpsa']:.2f} | "
              f"{r['hbd']} | {r['hba']} | {r['rotatable']} |" for r in rows]
    send(Markdown("\n".join(lines)))
    (names, smiles, formula) = (list([r[k] for r in rows]) for k in ('name', 'smiles', 'formula'))
    doubles = [np.array([r[k] for r in rows], dtype=np.float64) for k in ('mw', 'logp', 'tpsa')]
    ints = [np.array([r[k] for r in rows], dtype=np.int32) for k in ('hbd', 'hba', 'rotatable', 'nh_oh', 'n_o')]
    return Output(molecules=MoleculesT(
        names=names,
        smiles=smiles,
        formula=formula,
        mw=doubles[0],
        logp=doubles[1],
        tpsa=doubles[2],
        hbd=ints[0],
        hba=ints[1],
        rotatable=ints[2],
        nhOh=ints[3],
        nO=ints[4],
    ))
