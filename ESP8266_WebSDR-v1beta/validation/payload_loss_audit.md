# Revue ciblée de la perte d'un octet

Source examinée : candidat `ESP8266_WebSDR.ino`, SHA256
`1d17c5f48ef48fa055b8fc86dcacb24d2a86dd02b4bc980bc88727fcb7aceac0`.
Revue hors matériel, aucune modification de firmware et aucun accès série.

## Résultat

Aucun défaut de longueur ou chemin normal « count−1 » trouvé dans
`fillPayload`, `payloadCrc` ou `sendPayload`. La cause physique des pertes
observées reste non localisée. Cette revue ne démontre pas que tous les octets
placés dans le FIFO UART arrivent au PC.

Les quatre fonctions C++ exactes ont été extraites de la source gelée par
`build_exact_payload_audit.py`, puis compilées dans `exact_payload_audit.cpp`
avec `g++ -std=c++11 -O2 -Wall -Wextra -fsanitize=undefined,address`.
La sortie UART était remplacée par un collecteur mémoire ; aucune capture
matérielle n'a été faite par ce test.

Les 4094 combinaisons N=2..2048 / RAW ou CAP20 passent :

- comparaison à un empaquetage indépendant, bit par bit ;
- cinq capacités de sortie : 1, 63, 64, 65 et 128 octets ;
- appel réel de `sendPayload` et comparaison de chaque octet écrit ;
- contrôle que chaque bloc, y compris le dernier, appelle `finishTextChunk` ;
- cohérence du CRC calculé dans la première passe avec le bloc de référence ;
- curseur entièrement vidé en fin de génération, aucun diagnostic sanitizer.

Résultats aux tailles pertinentes :

| N | Format | Octets | Blocs envoyés | Dernier bloc |
| ---: | --- | ---: | ---: | ---: |
| 128 | RAW | 512 | 8 | 64 |
| 2047 | RAW | 8188 | 128 | 60 |
| 2047 | CAP20 | 5118 | 80 | 62 |
| 2048 | RAW | 8192 | 128 | 64 |
| 2048 | CAP20 | 5120 | 80 | 64 |

RAW ajoute 32 bits seulement lorsque le réservoir est vide. CAP20 ajoute
20 bits avec au plus quatre bits résiduels, donc le réservoir uint32 ne déborde
pas. La fin impaire est explicitement émise, quatre bits utiles complétés par
quatre zéros. La taille du tampon CRC de 128 octets et celle du tampon d'envoi
de 64 octets produisent exactement le même flux.

## Chaîne UART du cœur 3.1.2

Sources locales de la VM :
`/home/kaboom/.arduino15/packages/esp8266/hardware/esp8266/3.1.2/cores/esp8266/`.

- `HardwareSerial.h:191–194` : `write(buffer,size)` appelle `uart_write`.
- `uart.cpp:510–515` : `uart_do_write_char` attend que le FIFO ne soit pas
  plein puis écrit le caractère dans son registre.
- `uart.cpp:533–550` : le chemin normal boucle exactement `size` fois et
  retourne cette taille. Les chemins UART absent/désactivé ou GDB retournent
  zéro ; il n'existe pas de retour normal `size−1`.
- `uart.cpp:564–570` : `uart_wait_tx_empty` attend que le compte FIFO tombe à
  zéro. Il ne réinitialise pas le FIFO.
- `HardwareSerial.cpp:114–126` : `flush` appelle cette attente, puis ajoute un
  temps de caractère avec marge, calculé `bit_length * 1000000 / baud + 1`.
  `uart_get_bit_length`, `uart.cpp:213–216`, inclut start, données, parité et
  stop, soit dix bits en 8N1 : délai additionnel de 6 µs à 2 Mbauds, 11 µs à
  1 Mbaud et 87 µs à 115200.

Le sketch appelle en plus le délai PACE après chaque flush, y compris après le
dernier bloc. Il ne change ni débit ni paramètres UART à la fin d'un paquet.
Le FIFO n'est pas remis à zéro par cette séquence.

Les en-têtes DATA/BIN sont immédiatement suivis du premier bloc, sans pause
intermédiaire. C'est un écart observable par rapport aux blocs suivants, mais
ce n'est pas un défaut de longueur démontré : en-tête et premier bloc totalisent
moins de 128 octets, et `uart_do_write_char` attend de la place au besoin.
Ajouter une pause ici ne peut être présenté comme un correctif prouvé.

`sendPayload` incrémente `transmitErrors` si `Serial.write` retourne une taille
différente, puis abandonne sans insérer de texte dans le binaire. Les appelants
n'exploitent pas son booléen de retour ; le client doit détecter le paquet
incomplet par taille/CRC et se resynchroniser. Aucun chemin identifié n'explique
une perte exactement d'un octet avec `txErrors=0`.

## Choix de livraison

Au vu de la série matérielle 37/37 rapportée par le propriétaire du port à
115200, et des échecs à haut débit, la recommandation est de commencer les
sessions d'acquisition à 115200. La décision retenue conserve le firmware gelé
qui démarre à 2 Mbauds : la page effectue INFO puis BAUD115200 avant les réponses
longues et les captures. Les hauts débits restent disponibles comme options de
diagnostic, sans promesse de fiabilité sur la liaison testée.

Ce choix réduit le débit d'affichage mais ne change ni la cadence interne de
40 Méch/s nominaux ni la durée RF des instantanés.
