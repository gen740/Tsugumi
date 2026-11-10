import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import h5py
from scipy.signal import butter, filtfilt, welch
from scipy.signal.windows import tukey

from inputs.data.gw.GwFiles import GwFiles
import outputs.whitened.gw.WhitenedDir as out


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


def read(path):
    with h5py.File(path, "r") as f:
        h = f["strain"]["Strain"][()]
        dt = f["strain"]["Strain"].attrs["Xspacing"]
        t0 = f["meta"]["GPSstart"][()]
    return h, int(round(1 / dt)), float(t0)


def whiten(h, fs, f_low, f_high):
    f, psd = welch(h, fs=fs, nperseg=4 * fs)
    freqs = np.fft.rfftfreq(len(h), 1 / fs)
    hf = np.fft.rfft(h * tukey(len(h), 0.1))
    white = np.fft.irfft(hf / np.sqrt(np.interp(freqs, f, psd)), n=len(h))
    b, a = butter(4, [f_low, f_high], btype="band", fs=fs)
    return filtfilt(b, a, white)


def main(ctx):
    p = ctx.params()
    folder = ctx.input("data", GwFiles).Value().decode()
    names = sorted(n for n in os.listdir(folder) if n.endswith(".hdf5"))
    series, saved = [], {}
    for name in names:
        det = name.split("-")[0] + "1"
        h, fs, t0 = read(os.path.join(folder, name))
        w = whiten(h, fs, p.FLow(), p.FHigh())
        t = t0 + np.arange(len(h)) / fs - p.TEvent()
        saved[det] = w
        saved["t"] = t
        keep = (t > -p.Before()) & (t < p.After())
        w_std = w[(t > -10) & (t < -2)].std()
        series.append({"id": det, "label": f"{det} whitened ({p.FLow():g}-{p.FHigh():g} Hz)",
                       "x": t[keep].tolist(), "y": (w[keep] / w_std).tolist()})
        print(f"{det}: {len(h)} samples at {fs} Hz from GPS {t0:.0f}")
    np.savez(os.path.join(ctx.output_directory, "whitened.npz"), fs=fs, **saved)
    ctx.display_line_chart(series)
    emit(ctx, "whitened", out)
