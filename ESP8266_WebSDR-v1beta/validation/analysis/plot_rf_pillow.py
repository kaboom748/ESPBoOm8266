"""Tracés scientifiques statiques, axes calculés directement à partir des données."""
import json
import math
from pathlib import Path
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

report = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
font_path = "C:/Windows/Fonts/arial.ttf"
font = ImageFont.truetype(font_path, 21)
small = ImageFont.truetype(font_path, 18)
title = ImageFont.truetype(font_path, 28)
colors = ["#1a6eaa", "#e77725", "#219653", "#bf397a", "#6f53a9"]


def axes(im, rect, xlim, ylim, xticks, yticks, xlabel, ylabel, heading):
    d = ImageDraw.Draw(im)
    left, top, right, bottom = rect
    def xy(x, y):
        return (left+(x-xlim[0])/(xlim[1]-xlim[0])*(right-left),
                bottom-(y-ylim[0])/(ylim[1]-ylim[0])*(bottom-top))
    for x in xticks:
        px, _ = xy(x, ylim[0])
        d.line((px, top, px, bottom), fill="#e5e9ed", width=1)
        d.text((px, bottom+10), f"{x:g}", fill="#333333", font=small, anchor="mt")
    for y in yticks:
        _, py = xy(xlim[0], y)
        d.line((left, py, right, py), fill="#e5e9ed", width=1)
        d.text((left-12, py), f"{y:g}", fill="#333333", font=small, anchor="rm")
    d.line((left, top, left, bottom, right, bottom), fill="#444444", width=2)
    d.text((left, top-56), heading, font=title, fill="#182b3a")
    d.text((left, top-26), ylabel, font=small, fill="#444444")
    d.text(((left+right)/2, bottom+42), xlabel, font=font, fill="#333333", anchor="mt")
    return d, xy


im = Image.new("RGB", (1400, 800), "white")
d, xy = axes(im, (100, 115, 1340, 660), (-20, 20), (-110, -25),
             range(-20, 21, 5), range(-110, -24, 10),
             "Fréquence relative à RX (MHz), convention A - jB",
             "10 log10(PSD / 1 code^2/Hz)",
             "Accord fin : spectres de captures RF réelles")
for index, group in enumerate(report["fine_tune"]):
    spectrum = np.asarray(group["psd_counts2_per_hz"])
    f = np.fft.fftshift(np.fft.fftfreq(len(spectrum), 1/40000000))/1e6
    db = 10*np.log10(np.maximum(spectrum, 1e-16))
    db = np.convolve(db, np.ones(5)/5, mode="same")
    points = [xy(float(a), max(-110, min(-25, float(b)))) for a, b in zip(f[2:-2], db[2:-2])]
    d.line(points, fill=colors[index], width=2)
    label = f"RX {group['frequency_mhz']} MHz : 10 blocs"
    if index == 4:
        label += " (retour)"
    d.line((160, 525+index*29, 200, 525+index*29), fill=colors[index], width=3)
    d.text((210, 525+index*29), label, font=small, fill="#333333", anchor="lm")
d.text((100, 754), "TX fixe canal 11 ; gain RX 5 ; N=2048 ; Fs nominal 40 Méch/s ; aucune calibration en dBm.",
       font=small, fill="#555555")
im.save(out/"accord_fin_psd.png")

im = Image.new("RGB", (1400, 1000), "white")
gains = report["gains"]
for panel, (rect, ylim, yticks, heading, ylabel) in enumerate([
    ((100, 110, 1320, 450), (0, 6), range(7),
     "Gain PBUS : puissance et saturation mesurées", "log10(p95 puissance DC retirée / 1 code²)"),
    ((100, 610, 1320, 900), (0, 45), range(0, 46, 10),
     "Fraction moyenne des valeurs A/B aux rails observés -256 / +255", "Pourcentage (%)"),
]):
    d, xy = axes(im, rect, (0, 29), ylim, [0, 5, 10, 15, 20, 25, 29], yticks,
                 "Indice PBUS (pas de conversion en dB)", ylabel, heading)
    for noise, color, label in [(False, colors[0], "TX actif : 20 blocs / gain"),
                                 (True, "#777777", "TX arrêté : 6 blocs / gain")]:
        points = []
        for g in gains:
            row = g["tx_off"] if noise else g
            value = math.log10(max(row["power_p95"], 1e-12)) if panel == 0 else 100*row["rail_fraction_mean"]
            points.append(xy(g["gain"], value))
        d.line(points, fill=color, width=3)
        for px, py in points:
            d.ellipse((px-5, py-5, px+5, py+5), fill=color)
        y = rect[1]+22+(28 if noise else 0)
        d.line((900, y, 940, y), fill=color, width=3)
        d.text((950, y), label, font=small, fill="#333333", anchor="lm")
im.save(out/"gain_mesure.png")

im = Image.new("RGB", (1200, 850), "white")
d, xy = axes(im, (120, 145, 1120, 680), (-.15, 3.2), (-.15, 3.2),
             [0, 1, 2, 3], [0, 1, 2, 3],
             "Diminution demandée du centre RX depuis 2462 MHz (MHz)",
             "Déplacement mesuré du motif RF (MHz)",
             "Accord fin confirmé par le déplacement d'un repère spectral")
d.line([xy(0, 0), xy(3.15, 3.15)], fill="#cccccc", width=3)
for index, group in enumerate(report["fine_tune"]):
    measured = group["rf_feature_shift"]["shift_hz"]/1e6
    requested = group["requested_shift_hz"]/1e6
    px, py = xy(requested, measured)
    d.ellipse((px-7, py-7, px+7, py+7), fill=colors[index])
    label = f"{measured:+.6f} MHz"
    if index == 4:
        label = "Retour 2462 : +0.000000 MHz"
    right_side = requested > 2
    d.text((px-13 if right_side else px+13, py+(-50 if index == 4 else -18)),
           label, font=small, fill=colors[index], anchor="rm" if right_side else "lm")
d.text((120, 770), "Recherche libre du motif -3.906 à +4.883 MHz ; résolution 19.531 kHz ; gain RX 5.",
       font=small, fill="#444444")
d.text((120, 800), "Mesure relative, sans calibration absolue ; cinq séries réelles de dix blocs CRC valides.",
       font=small, fill="#444444")
im.save(out/"accord_fin_mesure.png")
