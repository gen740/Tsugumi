"""Opt-in cadence budget; shared runner throughput is not ordinary verification."""
def verify(times, interval):
    gaps = [b - a for a, b in zip(times[1:3], times[2:4])]
    assert all(interval - 0.1 <= gap <= interval + 1 for gap in gaps), gaps
