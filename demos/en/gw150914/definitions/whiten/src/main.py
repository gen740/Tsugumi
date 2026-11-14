import os

import numpy as np
import h5py
from scipy.signal import butter, filtfilt, welch
from scipy.signal.windows import tukey


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


def main(inputs, report, run):
    p = inputs.params
    folder = inputs.data.value.decode()
    names = sorted(n for n in os.listdir(folder) if n.endswith(".hdf5"))
    series, saved = [], {}
    for name in names:
        det = name.split("-")[0] + "1"
        h, fs, t0 = read(os.path.join(folder, name))
        w = whiten(h, fs, p.fLow, p.fHigh)
        t = t0 + np.arange(len(h)) / fs - p.tEvent
        saved[det] = w
        saved["t"] = t
        keep = (t > -p.before) & (t < p.after)
        w_std = w[(t > -10) & (t < -2)].std()
        series.append({"id": det, "label": f"{det} whitened ({p.fLow:g}-{p.fHigh:g} Hz)",
                       "x": t[keep].tolist(), "y": (w[keep] / w_std).tolist()})
        report.log(f"{det}: {len(h)} samples at {fs} Hz from GPS {t0:.0f}")
    np.savez(os.path.join(run.output_directory, "whitened.npz"), fs=fs, **saved)
    report.line_chart(series)
    return {'whitened': {"value": run.output_directory}}
