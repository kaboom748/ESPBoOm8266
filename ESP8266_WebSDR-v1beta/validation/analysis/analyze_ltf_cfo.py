"""Recherche hors ligne d'un L-LTF connu et mesure de sa fréquence dans A-jB.

Le balayage ne reçoit pas la fréquence attendue du firmware. La périodicité
128samples donne la CFO résiduelle ; le template lève l'ambiguïté312.5kHz.
Utilise le template et le décodeur SIGNAL déjà vérifiés de ce projet.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import numpy as np
from analyze_psd import load


def load_decoder(path):
    spec = importlib.util.spec_from_file_location("existing_signal_decoder", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def estimate(z, template, fs=40000000, maximum=5000000, minimum_rho=.60):
    if len(z) < 256:
        return {"accepted": False, "reason": "short"}
    t = np.arange(len(z))
    z = z-np.mean(z)
    energy = np.convolve(np.abs(z)**2, np.ones(128), mode="valid")
    denom = np.sqrt(np.maximum(energy, 1e-30)*np.vdot(template, template).real)
    best = None
    # Pas égal à la demi-largeur de l'ambiguïté CFO LTF.
    for frequency in np.arange(-maximum, maximum+1, fs/256):
        shifted = z*np.exp(-2j*np.pi*frequency*t/fs)
        corr = np.abs(np.correlate(shifted, template, mode="valid"))/denom
        pair = np.minimum(corr[:-128], corr[128:])
        start = int(np.argmax(pair))
        rho = float(pair[start])
        if best is None or rho > best["coarse_rho"]:
            best = {"start": start, "coarse_hz": float(frequency), "coarse_rho": rho}
    start = best["start"]
    shifted = z*np.exp(-2j*np.pi*best["coarse_hz"]*t/fs)
    x, y = shifted[start:start+128], shifted[start+128:start+256]
    repeat = np.vdot(x, y)/max(float(np.linalg.norm(x)*np.linalg.norm(y)), 1e-30)
    residual = float(np.angle(repeat)*fs/(2*np.pi*128))
    measured = best["coarse_hz"]+residual
    corrected = z*np.exp(-2j*np.pi*measured*t/fs)
    pair_rho = [float(abs(np.vdot(template, corrected[s:s+128]))/
                      max(float(np.linalg.norm(template)*np.linalg.norm(corrected[s:s+128])), 1e-30))
                for s in (start, start+128)]
    return {**best, "measured_cfo_hz": measured, "residual_hz": residual,
            "repeat_rho": float(abs(repeat)), "template_pair_rho": pair_rho,
            "accepted": bool(min(pair_rho) >= minimum_rho and abs(repeat) >= .85)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("files", nargs="+")
    p.add_argument("--decoder", type=Path,
                   default=Path(__file__).resolve().parents[2]/"outputs/capture_iq/decode_signal.py")
    p.add_argument("--out", required=True)
    p.add_argument("--range-hz", type=float, default=5000000)
    args = p.parse_args()
    decoder = load_decoder(args.decoder)
    rows = []
    for name in args.files:
        z = load(name)
        row = {"file": name, **estimate(z, decoder.LTF, maximum=args.range_hz)}
        if row["accepted"] and row["start"]+416 <= len(z):
            corrected = z*np.exp(-2j*np.pi*row["measured_cfo_hz"]*np.arange(len(z))/40000000)
            try:
                decoded = decoder.decode_at(corrected, row["start"], 40000000, False)
                row["signal"] = {"valid": decoded["valide_strict"],
                                 "rate_Mbps": decoded["debit_Mbps"],
                                 "length": decoded["longueur_octets"],
                                 "hard_errors": decoded["reencodage_distance_hard"]}
            except (ValueError, KeyError) as exc:
                row["signal_error"] = str(exc)
        rows.append(row)
        if row["accepted"]:
            print(json.dumps(row), flush=True)
    accepted = [r["measured_cfo_hz"] for r in rows if r["accepted"]]
    report = {"orientation": "A-jB", "fs_nominal_hz": 40000000,
              "frequency_prior_from_firmware": False, "rows": rows,
              "accepted": len(accepted),
              "median_cfo_hz": float(np.median(accepted)) if accepted else None,
              "cfo_min_max_hz": [min(accepted), max(accepted)] if accepted else None}
    Path(args.out).write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({k:v for k,v in report.items() if k != "rows"}))


if __name__ == "__main__":
    main()
