"""Seeded random walks: each walker steps ±1 every step, starting at 0."""
import numpy as np


def main(inputs):
    s = inputs.walk
    walkers, steps = s.walkers, s.steps
    rng = np.random.default_rng(s.seed)
    moves = rng.choice([-1.0, 1.0], size=(walkers, steps))
    positions = np.concatenate([np.zeros((walkers, 1)), np.cumsum(moves, axis=1)], axis=1)
    vec = positions.ravel()
    return {'walks': {
        'walkers': walkers,
        'steps': steps,
        'positions': vec,
    }}
