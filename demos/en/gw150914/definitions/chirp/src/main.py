import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.signal import spectrogram


def png(fig):
    import io
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=110, bbox_inches="tight")
    plt.close(fig)
    return buf.getvalue()


def main(inputs, report, run):
    p = inputs.params
    d = np.load(os.path.join(inputs.whitened.value.decode(), "whitened.npz"))
    fs, t, h1, l1 = int(d["fs"]), d["t"], d["H1"], d["L1"]
    near = (t > -0.2) & (t < 0.05)
    # The time shift between the sites (light travel time, at most 10 ms) and the sign of L1.
    best = max(((lag, s) for lag in range(-int(0.01 * fs), int(0.01 * fs) + 1) for s in (1, -1)),
               key=lambda x: np.dot(h1[near], x[1] * np.roll(l1, x[0])[near]))
    shift_ms, sign = 1000 * best[0] / fs, best[1]
    l1s = sign * np.roll(l1, best[0])
    rows = []
    for det, w in (("H1", h1), ("L1", l1)):
        noise = w[(t > -10) & (t < -2)].std()
        i = np.argmax(np.abs(w[(t > -0.1) & (t < 0.05)]))
        peak_t = t[(t > -0.1) & (t < 0.05)][i]
        snr = np.abs(w[(t > -0.1) & (t < 0.05)]).max() / noise
        rows.append((det, f"{peak_t * 1000:+.1f} ms", f"{snr:.1f}"))
    fig, (a, b) = plt.subplots(2, 1, figsize=(7.5, 6.2), sharex=True)
    a.plot(t[near], h1[near], label="H1", lw=1)
    a.plot(t[near], l1s[near], label=f"L1 shifted {shift_ms:+.1f} ms, x{sign}", lw=1, alpha=0.8)
    a.set_ylabel("whitened strain")
    a.legend(loc="upper left", fontsize=8)
    a.set_title("GW150914  peak SNR (rough)  " + "   ".join(f"{d}: {s}" for d, _, s in rows), fontsize=9)
    seg = (t > -1) & (t < 0.5)
    f, tt, S = spectrogram(h1[seg], fs=fs, nperseg=p.nperseg, noverlap=int(p.nperseg * 0.9))
    band = (f > 20) & (f < 500)
    b.pcolormesh(tt + t[seg][0], f[band], np.log10(S[band] + 1e-30), shading="auto", cmap="viridis")
    b.set_xlim(-0.2, 0.05)
    b.set_ylabel("frequency [Hz]")
    b.set_xlabel("time from GPS 1126259462.4 [s]")
    data = png(fig)
    with open(os.path.join(run.output_directory, "chirp.png"), "wb") as fh:
        fh.write(data)
    with open(os.path.join(run.output_directory, "snr.csv"), "w") as fh:
        fh.write("detector,peak_time,snr\n" + "".join(f"{d},{pt},{s}\n" for d, pt, s in rows))
    report.log(f"L1 vs H1: shift {shift_ms:+.1f} ms, sign {sign}; " + ", ".join(f"{d} SNR {s}" for d, _, s in rows))
    report.image(data, "image/png", "GW150914: H1/L1 overlay and H1 spectrogram")
    return {'plots': {"value": run.output_directory}}
