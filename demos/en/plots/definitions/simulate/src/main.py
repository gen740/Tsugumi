"""Seeded random walks: each walker steps ±1 every step, starting at 0."""

from card import Input, Output, params
from walk.Walks import WalksT

import numpy as np


def run(_input: Input) -> Output:
    s = params
    walkers, steps = s.walkers, s.steps
    rng = np.random.default_rng(s.seed)
    moves = rng.choice([-1.0, 1.0], size=(walkers, steps))
    positions = np.concatenate([np.zeros((walkers, 1)), np.cumsum(moves, axis=1)], axis=1)
    vec = positions.ravel()
    return Output(walks=WalksT(
        walkers=walkers,
        steps=steps,
        positions=vec,
    ))
