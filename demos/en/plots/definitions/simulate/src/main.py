"""Seeded random walks: each walker steps ±1 every step, starting at 0."""
import numpy as np

from inputs.walk.walk.Settings import Settings
import outputs.walks.walk.Walks as out


def main(ctx):
    s = ctx.input("walk", Settings)
    walkers, steps = s.Walkers(), s.Steps()
    rng = np.random.default_rng(s.Seed())
    moves = rng.choice([-1.0, 1.0], size=(walkers, steps))
    positions = np.concatenate([np.zeros((walkers, 1)), np.cumsum(moves, axis=1)], axis=1)
    b = ctx.builder(positions.size * 8 + 1024)
    vec = b.CreateNumpyVector(positions.ravel())
    out.Start(b)
    out.AddWalkers(b, walkers)
    out.AddSteps(b, steps)
    out.AddPositions(b, vec)
    ctx.output("walks", b, out.End(b))
