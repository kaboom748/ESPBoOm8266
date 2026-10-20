# Reproduire les graphiques hors ligne

Ces scripts sont des outils de vérification facultatifs. Seuls le HTML et
le sketch Arduino sont nécessaires pour utiliser le récepteur.

Avec Python, NumPy et Pillow :

1. Extraire `../rf/rf_evidence.tgz` dans un dossier de travail. Le dossier de
   base attendu contient `v2_rf_reliable_short`, `v2_rf_fine_remaining`
   et `v2_gain_short`.
2. Recalculer les mesures :

   ```text
   python analyze_rf_campaign.py DOSSIER_DE_BASE --out RESULTATS --decoder decode_signal.py
   ```

3. Tracer les graphiques sous Windows :

   ```text
   python plot_rf_pillow.py RESULTATS/resume_rf.json GRAPHIQUES
   ```

Le traceur utilise Arial dans `C:/Windows/Fonts/arial.ttf`. Sur un autre système,
adapter uniquement ce chemin à une police locale.

Le champ `fine_tune[].rf_feature_shift` mesure le déplacement du motif RF.
Les corrélations globales conservées pour diagnostic sont biaisées par le
filtre fixe du récepteur ; elles ne constituent pas le résultat retenu.
Les fichiers bruts portent leurs CRC et leurs métadonnées dans l'archive.
