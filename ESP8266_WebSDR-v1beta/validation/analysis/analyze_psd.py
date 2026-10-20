"""PSD de blocs I/Q sauvegardés, hors ligne. Unités : counts²/Hz, jamais dBm.

Exemple : python3 analyze_psd.py --out psd.json captures/*.u32le
Option --reference compare les spectres moyens par corrélation et rapporte
un décalage fréquentiel candidat, pas une mesure absolue du PLL.
"""
import argparse
import json
from pathlib import Path


def load(path):
    import numpy as np
    path = Path(path)
    if path.suffix == ".packed20":
        from qualify_protocol import unpack20
        data = path.read_bytes()
        count = len(data)*8//20
        a, b = unpack20(data, count)
        return np.asarray(a, dtype=float) - 1j*np.asarray(b, dtype=float)
    words = np.frombuffer(path.read_bytes(), dtype="<u4")
    a = (((words & 1023).astype(np.int32) ^ 512) - 512).astype(float)
    b = ((((words >> 10) & 1023).astype(np.int32) ^ 512) - 512).astype(float)
    return a - 1j*b


def psd(z, sample_rate):
    import numpy as np
    if len(z) < 2:
        raise ValueError("Bloc trop court")
    window = np.hanning(len(z))
    if not np.sum(window*window):
        window = np.ones(len(z))
    centered = z - np.mean(z)
    power = np.abs(np.fft.fftshift(np.fft.fft(centered*window)))**2
    power /= sample_rate*np.sum(window*window)
    frequency = np.fft.fftshift(np.fft.fftfreq(len(z), 1/sample_rate))
    return frequency, power


def aggregate(files, sample_rate):
    import numpy as np
    powers = []
    for path in files:
        frequency, power = psd(load(path), sample_rate)
        if powers and len(power) != len(powers[0]):
            raise ValueError("Comparer des blocs de même taille")
        powers.append(power)
    mean = np.mean(powers, axis=0)
    peak = int(np.argmax(mean))
    return frequency, mean, {
        "files": [str(p) for p in files], "blocks": len(powers),
        "sample_rate_hz": sample_rate, "bins": len(mean),
        "bin_spacing_hz": sample_rate/len(mean),
        "peak_relative_hz": float(frequency[peak]),
        "power_integral_counts2": float(np.sum(mean)*sample_rate/len(mean)),
        "psd_units": "counts^2/Hz",
    }


def compare(reference, current, sample_rate):
    import numpy as np
    if len(reference) != len(current):
        raise ValueError("Tailles PSD différentes")
    # Log + centrage réduit la domination d'un seul pic ; éviter les bords
    # circulaires en corrélant linéairement. La crête doit être RF identifiable.
    eps = max(float(np.max(reference)), float(np.max(current)), 1)*1e-12
    ref = np.log10(reference + eps)
    cur = np.log10(current + eps)
    ref -= np.mean(ref)
    cur -= np.mean(cur)
    corr = np.correlate(cur, ref, "full")
    lags = np.arange(-len(ref)+1, len(ref))
    keep = np.abs(lags) <= len(ref)//4
    selected = np.flatnonzero(keep)[np.argmax(corr[keep])]
    score = corr[selected]/max(float(np.linalg.norm(cur)*np.linalg.norm(ref)), 1e-30)
    return {
        "candidate_shift_bins": int(lags[selected]),
        "candidate_shift_hz": float(lags[selected]*sample_rate/len(ref)),
        "correlation_score": float(score),
        "interpretation": "Candidat relatif du spectre observé; vérifier même signal RF, SNR et signe.",
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("files", nargs="+")
    p.add_argument("--reference", nargs="+")
    p.add_argument("--rate", type=float, default=40000000)
    p.add_argument("--out", required=True)
    a = p.parse_args()
    freq, power, report = aggregate(a.files, a.rate)
    report["frequency_hz"] = freq.tolist()
    report["psd_counts2_per_hz"] = power.tolist()
    if a.reference:
        _, ref, meta = aggregate(a.reference, a.rate)
        report["reference"] = meta
        report["comparison"] = compare(ref, power, a.rate)
    Path(a.out).write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({k:v for k,v in report.items()
                      if k not in ("frequency_hz", "psd_counts2_per_hz")}))


if __name__ == "__main__":
    main()
