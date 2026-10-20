"""Consolider les mesures RF réelles et produire leurs preuves locales."""
import argparse
import json
from collections import defaultdict
from pathlib import Path
import numpy as np
from analyze_psd import aggregate, compare, load
from analyze_ltf_cfo import estimate, load_decoder


def read_events(directory):
    return [json.loads(s) for s in (directory/"rx/events.jsonl").read_text(
        encoding="utf-8").splitlines()]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("base", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--decoder", type=Path, required=True)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    first = args.base/"v2_rf_reliable_short"
    rest = args.base/"v2_rf_fine_remaining"
    gains = args.base/"v2_gain_short"
    report = json.loads((gains/"report.json").read_text(encoding="utf-8"))
    report["fine_tune"] = []
    decoder = load_decoder(args.decoder)
    reference = None
    groups = [(first, 0, 2462), (rest, 0, 2461), (rest, 1, 2460),
              (rest, 2, 2459), (rest, 3, 2462)]
    all_events = {str(root): read_events(root) for root in (first, rest, gains)}
    report["transport_rejections"] = {
        Path(root).name: [e for e in events if e["event"] == "capture_rejected"]
        for root, events in all_events.items()}
    for number, (root, index, frequency) in enumerate(groups):
        selected = sorted((root/"strong").glob(f"fine_{index}_{frequency}_*.u32le"))
        f, power, psd_meta = aggregate(selected, 40000000)
        group = {"frequency_mhz": frequency, "sequence_index": number,
                 "strong_blocks": len(selected), "psd": psd_meta,
                 "psd_counts2_per_hz": power.tolist(),
                 "requested_shift_hz": (2462-frequency)*1e6}
        if reference is None:
            reference = power
        else:
            group["comparison"] = compare(reference, power, 40000000)
        events = all_events[str(root)]
        wires = {e["sha256"]: root/"rx"/e["file"]
                 for e in events if e["event"] == "wire" and e["direction"] == "rx"
                 and e["bytes"] == 5120}
        captures = [e for e in events if e["event"] == "capture"
                    and e["label"] == f"fine_{index}_{frequency}"]
        ltf = []
        for row in captures:
            wire = wires.get(row["payload_sha256"])
            if wire is None:
                continue
            from qualify_protocol import unpack20
            a, b = unpack20(wire.read_bytes(), 2048)
            z = np.asarray(a) - 1j*np.asarray(b)
            found = estimate(z, decoder.LTF)
            if not found["accepted"]:
                continue
            found.update({"wire_file": str(wire),
                          "payload_sha256": row["payload_sha256"],
                          "capture_sequence": row["status"]["seq"]})
            if found["start"]+416 <= len(z):
                corrected = z*np.exp(-2j*np.pi*found["measured_cfo_hz"]*np.arange(len(z))/40000000)
                try:
                    decoded = decoder.decode_at(corrected, found["start"], 40000000, False)
                    found["signal"] = {"valid": decoded["valide_strict"],
                        "rate_Mbps": decoded["debit_Mbps"], "length": decoded["longueur_octets"],
                        "hard_errors": decoded["reencodage_distance_hard"]}
                except (ValueError, KeyError) as exc:
                    found["signal_error"] = str(exc)
            ltf.append(found)
        group["valid_capture_count"] = len(captures)
        group["ltf_matches"] = ltf
        group["ltf_accepted"] = len(ltf)
        group["median_ltf_cfo_hz"] = float(np.median(
            [x["measured_cfo_hz"] for x in ltf])) if ltf else None
        group["strict_signal_count"] = sum(x.get("signal", {}).get("valid", False) for x in ltf)
        report["fine_tune"].append(group)
        print(json.dumps({k:v for k,v in group.items()
                          if k not in ("psd_counts2_per_hz", "psd", "ltf_matches")}), flush=True)
    ref_cfo = report["fine_tune"][0]["median_ltf_cfo_hz"]
    frequency_axis = np.fft.fftshift(np.fft.fftfreq(2048, 1/40000000))
    feature_bins = np.flatnonzero(np.abs(frequency_axis) <= 500000)
    template = 10*np.log10(report["fine_tune"][0]["psd_counts2_per_hz"])
    template = template[feature_bins]-np.mean(template[feature_bins])
    for group in report["fine_tune"]:
        measured = group["median_ltf_cfo_hz"]
        group["delta_ltf_cfo_hz"] = measured-ref_cfo if measured is not None and ref_cfo is not None else None
        db = 10*np.log10(group["psd_counts2_per_hz"])
        candidates = []
        # Même plage pour chaque consigne. Aucune fréquence attendue ne sert
        # à choisir la position : on suit le creux RF observé dans la référence.
        for shift in range(-200, 251):
            target = db[feature_bins+shift]
            target = target-np.mean(target)
            score = float(np.dot(template, target)/
                          max(float(np.linalg.norm(template)*np.linalg.norm(target)), 1e-30))
            candidates.append((score, shift))
        score, shift = max(candidates)
        group["rf_feature_shift"] = {
            "shift_hz": shift*40000000/2048, "correlation": score,
            "resolution_hz": 40000000/2048,
            "search_min_hz": -200*40000000/2048,
            "search_max_hz": 250*40000000/2048,
            "template_reference_interval_hz": [-500000, 500000],
            "expected_frequency_used_in_search": False,
            "error_vs_requested_hz": shift*40000000/2048-group["requested_shift_hz"],
            "method": "Corrélation normalisée de la PSD logarithmique autour du creux RF de la référence2462 ; pas la corrélation globale dominée par le filtre RX."}
    report["measurement_units"] = {
        "power": "codes^2 après retrait DC ; aucune calibration dBm",
        "psd": "codes^2/Hz ; Fs nominal40MHz",
        "cfo": "CFO LTF de A-jB, sans consigne de fréquence fournie au détecteur",
        "gain": "indice PBUS ; aucune conversion dB",
        "rails": "codes observés -256/+255 ; pas une preuve de résolution ADC"}
    report["qualification"] = {
        "capture_firmware": "v2, fine PLL + pacing, boot UART 2000000",
        "physical_test_centers_mhz": [2462, 2461, 2460, 2459, 2462],
        "physical_test_gain_indices": [0, 5, 10, 15, 20, 25, 29],
        "relative_frequency_shift_confirmed": True,
        "decision_field": "fine_tune[].rf_feature_shift",
        "global_psd_correlation_used_for_decision": False,
        "absolute_frequency_calibrated": False,
        "all_51_centers_rf_measured": False,
        "v3_followup": "Parent annonce uniquement débit initial115200 et commentaires, chemins I/Q/gain/PLL inchangés ; aucune nouvelle acquisition RF effectuée pour v3."}
    (args.out/"resume_rf.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
