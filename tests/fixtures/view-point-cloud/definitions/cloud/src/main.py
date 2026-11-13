import math
import struct


def main(ctx):
    # Little-endian float32 XYZ triples; no JSON expansion of point coordinates.
    points = bytearray()
    count = 12000
    for i in range(count):
        t = i * 0.025
        radius = 0.3 + 0.5 * (i / count)
        points.extend(struct.pack("<fff", radius * math.cos(t), (i / count - 0.5) * 1.6, radius * math.sin(t)))
    ctx.display_module("points", {"count": count}, {"points": points})
