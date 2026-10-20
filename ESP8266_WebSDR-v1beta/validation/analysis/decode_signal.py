#!/usr/bin/env python3
"""Décode le champ SIGNAL OFDM d'un dump uint32LE de capture_iq.py.

Usage : python3 decode_signal.py capture.bin --fs 40000000
Nécessite NumPy. Aucun accès matériel, aucun décodage du contenu MAC.
Les séquences d'apprentissage et paramètres du code convolutif sont ceux
d'IEEE802.11, vérifiés avec les constantes de référence de gr-ieee802-11 :
https://github.com/bastibl/gr-ieee802-11/blob/maint-3.10/examples/wifi_phy_hier.grc
https://github.com/bastibl/gr-ieee802-11/blob/maint-3.10/lib/utils.cc
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import numpy as np

# Séquence fréquentielle L-LTF sur64 cases centrées, DC à l'indice32.
LTF64=np.array([0,0,0,0,0,0,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,
                0,1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1,0,0,0,0,0])
LTF_FREQ=np.zeros(128,dtype=complex)
LTF_FREQ[32:96]=LTF64
LTF=np.fft.ifft(np.fft.ifftshift(LTF_FREQ))
ACTIVE=np.r_[np.arange(-26,0),np.arange(1,27)]
PILOTS=np.array([-21,-7,7,21])
DATA_MASK=~np.isin(ACTIVE,PILOTS)
RATES={0xd:6,0xf:9,0x5:12,0x7:18,0x9:24,0xb:36,0x1:48,0x3:54}
INTERLEAVER=np.array([3*(k%16)+k//16 for k in range(48)])


def unpack(binary):
    if len(binary)%4 or len(binary)<416*4:
        raise ValueError("Au moins416 mots uint32LE sont nécessaires ; longueur multiple de4.")
    w=np.frombuffer(binary,dtype="<u4").astype(np.int64)
    if not np.all(w>>20==3):
        raise ValueError("Bits31:20 différents de0x3 dans le dump.")
    a=((w&1023)^512)-512
    b=(((w>>10)&1023)^512)-512
    return a+1j*b


def repeat_phase(z,start):
    x=z[start:start+128];y=z[start+128:start+256]
    x=x-x.mean();y=y-y.mean()
    den=np.sqrt(np.vdot(x,x).real*np.vdot(y,y).real)
    if den==0:raise ValueError("Énergie nulle dans les symboles d'apprentissage.")
    return np.vdot(x,y)/den


def template_scores(z,template):
    length=len(template)
    windows=np.lib.stride_tricks.sliding_window_view(z,length)
    energy=(abs(windows)**2).sum(axis=1)-abs(windows.sum(axis=1))**2/length
    numerator=abs(windows@template.conj())
    denominator=np.sqrt(np.maximum(energy,0)*np.vdot(template,template).real)
    return np.divide(numerator,denominator,out=np.zeros_like(numerator),where=denominator>0)


def detect_ltf(z):
    candidates=[]
    if len(z)<416:return candidates
    for conjugate in (False,True):
        template=LTF.conj() if conjugate else LTF
        scores=template_scores(z,template)
        pairs=np.minimum(scores[:-128],scores[128:])
        for start in range(len(z)-416+1):
            score=float(pairs[start])
            if score<0.65:continue
            if start>0 and pairs[start-1]>score:continue
            if start+1<len(pairs) and pairs[start+1]>score:continue
            candidates.append({"start":start,"conjugate":conjugate,
                               "template_pair_min_rho":score})
    return sorted(candidates,key=lambda row:-row["template_pair_min_rho"])


def encode(bits):
    state=0;output=[]
    # Polynômes155/117 octal avec entrée dans le bit de poids faible.
    # Équivalents aux133/171 octal lorsque l'ordre des registres est inversé.
    for bit in bits:
        state=((state<<1)|int(bit))&127
        output.extend([(state&0o155).bit_count()%2,(state&0o117).bit_count()%2])
    return np.array(output)


def viterbi(soft):
    metrics=np.full(64,np.inf);metrics[0]=0
    paths=[None]*64;paths[0]=[]
    for offset in range(0,len(soft),2):
        newer=np.full(64,np.inf);newpaths=[None]*64
        for state in range(64):
            if not np.isfinite(metrics[state]):continue
            for bit in (0,1):
                register=(state<<1)|bit
                code=np.array([(register&0o155).bit_count()%2,
                               (register&0o117).bit_count()%2])
                cost=float(((soft[offset:offset+2]-(2*code-1))**2).sum())
                nxt=register&63
                if metrics[state]+cost<newer[nxt]:
                    newer[nxt]=metrics[state]+cost
                    newpaths[nxt]=paths[state]+[bit]
        metrics=newer;paths=newpaths
    # L'état final est libre : les bits de terminaison seront contrôlés ensuite.
    end=int(np.argmin(metrics))
    return np.array(paths[end]),float(metrics[end]),end


def decode_at(z,start,fs,conjugate):
    if start<0 or start+416>len(z):
        raise ValueError("Fenêtre LTF+SIGNAL complète absente à l'indice demandé.")
    repetition=repeat_phase(z,start)
    omega=float(np.angle(repetition)/128)
    corrected=z*np.exp(-1j*omega*np.arange(len(z)))
    if conjugate:corrected=corrected.conj()
    fft=lambda x:np.fft.fftshift(np.fft.fft(x))
    train1=fft(corrected[start:start+128])
    train2=fft(corrected[start+128:start+256])
    channel=(train1[ACTIVE+64]+train2[ACTIVE+64])/(2*LTF_FREQ[ACTIVE+64])
    if np.any(abs(channel)<1e-12):raise ValueError("Canal estimé singulier.")
    signal_start=start+288
    signal=fft(corrected[signal_start:signal_start+128])[ACTIVE+64]/channel
    pilots=signal[np.isin(ACTIVE,PILOTS)]
    phase=float(np.angle(np.sum(pilots*np.array([1,1,1,-1]))))
    signal*=np.exp(-1j*phase)
    carriers=signal[DATA_MASK]
    soft=carriers.real[INTERLEAVER]
    bits,cost,final_state=viterbi(soft)
    hard=(soft>0).astype(int)
    hard_distance=int(np.count_nonzero(hard!=encode(bits)))
    rate=int("".join(map(str,bits[:4])),2)
    length=sum(int(bits[5+k])<<k for k in range(12))
    checks={
        "rate_valide":rate in RATES,
        "reserved_zero":bool(bits[4]==0),
        "parite_paire_ok":bool(bits[:18].sum()%2==0),
        "tail_six_zeros":bool(np.all(bits[18:]==0)),
        "reencodage_48bits_sans_erreur":hard_distance==0,
    }
    return {
        "valide_strict":all(checks.values()),
        "LTF_premier_indice":start,"LTF_second_indice":start+128,
        "SIGNAL_fft_premier_indice":signal_start,
        "LTF_correlation_repetition":float(abs(repetition)),
        "conjugaison_A_plus_jB":conjugate,
        "CFO_A_plus_jB_hz":omega*fs/(2*np.pi),
        "CFO_convention_decodec_hz":(-1 if conjugate else 1)*omega*fs/(2*np.pi),
        "CFO_limite":"Signe relatif à la convention complexe indiquée ; ambiguïté moduloFs/128.",
        "bits_SIGNAL":bits.tolist(),"RATE_hex":hex(rate),
        "debit_Mbps":RATES.get(rate),"longueur_octets":length,
        "controles":checks,"viterbi_etat_initial":0,
        "viterbi_etat_final_libre":final_state,
        "reencodage_distance_hard":hard_distance,"metrique_viterbi_soft":cost,
        "BPSK_erreur_RMS_normalisee":float(np.sqrt(np.mean(abs(carriers-np.where(carriers.real>0,1,-1))**2))),
        "signal_egalise_reel":carriers.real.tolist(),
        "signal_egalise_imag":carriers.imag.tolist(),
        "limite":"Champ SIGNAL seulement ; aucun contenu MAC ou payload décodé.",
    }


def analyze(z,fs,manual_start=None):
    candidates=detect_ltf(z) if manual_start is None else [
        {"start":manual_start,"conjugate":False},
        {"start":manual_start,"conjugate":True}]
    results=[]
    for candidate in candidates:
        decoded=decode_at(z,candidate["start"],fs,candidate["conjugate"])
        decoded["detection"]=candidate
        results.append(decoded)
    return {
        "Fs_hz_utilisee":fs,
        "FFT_taille":128,
        "hypothese":"OFDM20MHz échantillonné à40Méch/s ; Fs est fourni par l'utilisateur.",
        "nombre_candidats_LTF":len(candidates),
        "en_tetes_valides":[r for r in results if r["valide_strict"]],
        "candidats_rejetes":[r for r in results if not r["valide_strict"]],
        "aucun_debit_ou_longueur_attendus_imposes":True,
    }


def main():
    # JSON UTF-8 déterministe, y compris lorsque stdout est un pipe sous Windows.
    if hasattr(sys.stdout,"reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary",type=Path)
    parser.add_argument("--fs",type=float,default=40e6,
                        help="Cadence en Hz, valeur par défaut40000000.")
    parser.add_argument("--ltf-start",type=int,
                        help="Indice du début du premier LTF complet, sinon détection automatique.")
    parser.add_argument("--json",type=Path,help="Sauvegarde optionnelle du résultat JSON.")
    args=parser.parse_args()
    if not np.isfinite(args.fs) or args.fs<=0:parser.error("--fs doit être positif et fini.")
    binary=args.binary.read_bytes()
    result=analyze(unpack(binary),args.fs,args.ltf_start)
    result["fichier_source"]=str(args.binary)
    result["source_sha256"]=hashlib.sha256(binary).hexdigest()
    text=json.dumps(result,ensure_ascii=False,indent=2)+"\n"
    if args.json:
        if args.json.resolve()==args.binary.resolve():
            parser.error("Le JSON ne peut pas remplacer le binaire source.")
        with args.json.open("x",encoding="utf-8") as handle:handle.write(text)
    print(text,end="")
    return 0 if result["en_tetes_valides"] else 2


if __name__=="__main__":
    try:
        sys.exit(main())
    except (ValueError,OSError) as exc:
        print("{}: {}".format(type(exc).__name__,exc),file=sys.stderr)
        sys.exit(1)
