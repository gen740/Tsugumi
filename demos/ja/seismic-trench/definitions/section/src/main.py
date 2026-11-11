import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import csv

from inputs.data.quake.QuakeFiles import QuakeFiles
import outputs.section.quake.SectionDir as out


def emit(ctx, port, out):
    b = ctx.builder()
    s = b.CreateString(ctx.output_directory)
    out.Start(b)
    out.AddValue(b, s)
    ctx.output(port, b, out.End(b))


def png(fig):
    import io
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=110, bbox_inches="tight")
    plt.close(fig)
    return buf.getvalue()


def main(ctx):
    p = ctx.params()
    folder = ctx.input("data", QuakeFiles).Value().decode()
    rows = list(csv.DictReader(open(os.path.join(folder, sorted(os.listdir(folder))[0]))))
    ev = np.array([(float(r["latitude"]), float(r["longitude"]), float(r["depth"]), float(r["mag"]))
                   for r in rows if r.get("type") == "earthquake" and r["mag"] and r["depth"]])
    band = (ev[:, 0] >= p.LatMin()) & (ev[:, 0] <= p.LatMax())
    sel = ev[band]
    fig, (m, s) = plt.subplots(1, 2, figsize=(10, 4.4), gridspec_kw={"width_ratios": [1, 2.2]})
    m.scatter(ev[:, 1], ev[:, 0], s=1, c="#bbb")
    m.scatter(sel[:, 1], sel[:, 0], s=1, c="#E5484D")
    m.set_xlabel("longitude")
    m.set_ylabel("latitude")
    m.set_title(f"{len(ev)} events, band {p.LatMin():g}-{p.LatMax():g} N", fontsize=9)
    sc = s.scatter(sel[:, 1], sel[:, 2], s=(sel[:, 3] - 3) ** 2 * 3, c=sel[:, 3], cmap="plasma", alpha=0.6)
    s.invert_yaxis()
    s.set_xlabel("longitude")
    s.set_ylabel("depth [km]")
    s.set_title("cross-section (subducting Pacific slab)", fontsize=9)
    fig.colorbar(sc, ax=s, label="M")
    data = png(fig)
    with open(os.path.join(ctx.output_directory, "section.png"), "wb") as fh:
        fh.write(data)
    np.savetxt(os.path.join(ctx.output_directory, "selected.csv"), sel, delimiter=",",
               header="latitude,longitude,depth_km,mag", comments="", fmt="%.4f")
    print(f"{len(sel)} of {len(ev)} events between {p.LatMin():g} and {p.LatMax():g} N")
    ctx.display_image(data, "image/png", "Japan Trench: events and depth cross-section")
    emit(ctx, "section", out)
