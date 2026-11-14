"""Seeded random walks: each walker steps ±1 every step, starting at 0."""
import tsugumi
import numpy as np

from inputs.walk.walk.Settings import Settings


def main(inputs):
    s = inputs.input("walk", Settings)
    walkers, steps = s.Walkers(), s.Steps()
    rng = np.random.default_rng(s.Seed())
    moves = rng.choice([-1.0, 1.0], size=(walkers, steps))
    positions = np.concatenate([np.zeros((walkers, 1)), np.cumsum(moves, axis=1)], axis=1)
    vec = positions.ravel()
    _out_value = {}
    _out_value['walkers'] = walkers
    _out_value['steps'] = steps
    _out_value['positions'] = vec
    return {'walks': _out_value}
