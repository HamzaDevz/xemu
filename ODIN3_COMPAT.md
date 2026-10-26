# Compatibilité Odin 3 : xemu (Xbox)

Notre propre liste de compatibilité, tenue d'après nos tests sur l'AYN Odin 3 (Snapdragon 8 Elite, Adreno 830) avec notre build de la branche `odin3`. Chaque test met à jour la ligne du jeu : statut, mesure, date et commit testé.

Autres émulateurs : [Eden (Switch)](https://github.com/HamzaDevz/eden/blob/odin3/ODIN3_COMPAT.md) · [Azahar (3DS)](https://github.com/HamzaDevz/azahar/blob/odin3/ODIN3_COMPAT.md) · [Dolphin (GC/Wii)](https://github.com/HamzaDevz/dolphin/blob/odin3/ODIN3_COMPAT.md) · [Cemu (Wii U)](https://github.com/HamzaDevz/Cemu/blob/odin3/ODIN3_COMPAT.md) · [Vita3K (Vita)](https://github.com/HamzaDevz/Vita3K/blob/odin3/ODIN3_COMPAT.md) · [aPS3e (PS3)](https://github.com/HamzaDevz/aps3e/blob/odin3/ODIN3_COMPAT.md) · [Bachata S4 (PS4)](https://github.com/HamzaDevz/bachata-s4/blob/odin3/ODIN3_COMPAT.md) · [xemu (Xbox)](https://github.com/HamzaDevz/xemu/blob/odin3/ODIN3_COMPAT.md)

## Échelle

| Statut | Sens |
|---|---|
| 🟢 Parfait | vu en partie, plein régime, aucun défaut vu |
| 🔵 Jouable | vu en partie, défauts mineurs |
| 🟡 Problèmes | vu en partie, défauts gênants (fps, graphismes, son) |
| 🟠 Menus/intro | démarre, pas encore vu en partie |
| 🔴 Ne marche pas | crash, écran noir ou blocage |
| ⚪ Non testé | présent sur la console, jamais lancé |

## Comment on teste

Paquet `com.izzy2lost.x1box.odin3` (X1 BOX, port Android de xemu), comparé à `com.izzy2lost.x1box.master` pour l'amont. Banc adb dans `tools/odin3/` : `run.sh` lance le disque et joue le scénario `survey.txt` (boot 20 s, intro 40 s sans toucher, Start/A dans les menus, puis ~44 s de jeu avec A toutes les 10 s) ; `poll.sh` relève la puissance et les températures ; une capture par phase. Console refroidie avant chaque run (GPU < 40 °C). Réglages par défaut (Vulkan), `install.sh` force le 60 Hz dans l'EEPROM pour les disques PAL. « fps » = images réellement affichées.

## Jeux

| Jeu | Statut | Vu jusqu'à | Mesure | Notes | Testé le | Build |
|---|---|---|---|---|---|---|
| Burnout 3 Takedown (EU) | 🟠 Menus/intro | Menu de profil après l'intro | Joueurs façon Switch vérifiés (log « Connected … to port N ») : manette externe J1 et passe l'intro, 2e manette (dongle Xbox simulé) J2, Odin en plus J3 ; Odin seule J1 au lancement et passe l'intro | Lancé par ES-DE ; les manettes rejoignent à l'appui, J1 rendu à l'Odin à la sortie du dock ou au départ de sa manette ; changement de jeu depuis ES-DE (Burnout 3, ES-DE, Capcom Classics 1, ES-DE, Burnout 3) : chaque autre jeu démarre dans un processus neuf ; relancer le même jeu reprend le même processus mais l'image reste noire, car la surface Vulkan n'est pas recréée au retour au premier plan (bug déjà présent : toute reprise après un passage en arrière-plan) | 2026-10-26 | 29c8be5987 |
| Crazy Taxi 3 (EU) | 🔵 Jouable | en jeu, ligne de départ | 59,1 fps en jeu, 7,8 W, CPU 92 °C ; intro 53 fps | intro vidéo saccadée ; jamais vu en pleine course | 2026-10-18 | 6a72f0528e (série s3) |
| MotoGP (EU) | 🔵 Jouable | en course, tour 1 (moto à l'arrêt) | 56,2 fps en course, 6,3 % d'images lentes, 8,9 W, CPU 104 °C | limité par le CPU, n'atteint pas 60 ; x87 et cache (4baf65b0ac, 6a72f0528e) : course 52 → 56,5 fps | 2026-10-18 | 6a72f0528e |
| Metal Slug 3 (EU) | 🔵 Jouable | début de la mission 1 | 59,7 fps, 0,08 % d'images lentes, 6,3 W, CPU 64,8 °C, GPU 45,1 °C | décor et quatre portraits complets ; 6c13ef471b propage l'invalidation à toutes les textures partageant une page VRAM | 2026-10-19 | 6c13ef471b |
| Metal Slug 4 (USA, EU) | 🟡 Problèmes | en jeu | 59,1 fps, 6,1 W, CPU 70 °C | la majeure partie du décor est noire (même bug que MS3) | 2026-10-18 | 4e2b2f34c9 |
| Metal Slug 5 (USA) | 🟡 Problèmes | en jeu | 57,5 fps, 6,7 W, CPU 90 °C | tuiles noires ; seul jeu où l'amont fait mieux (59,1 fps, mais 11,9 W) | 2026-10-18 | 6a72f0528e (série s3) |
| Ninja Gaiden (EU) | 🔵 Jouable | début du chapitre 1, vallée et cascade ; déplacements, sauts et attaques | 58,3 fps sur 19,8 s, 4,9 % d’images lentes, pire 83,3 ms | enfin mesuré en jeu (`codex-ng-real/gameplay`) ; cible native 60, pas encore un 60 stable ; image correcte sur le passage, suite non testée | 2026-10-19 | 6a72f0528e |
| Conker: Live & Reloaded (EU) | 🟠 Menus/intro | logo Rare, menu profil | intro 59,8 fps, menu 29,9 fps, 2,9 W | l'image `.xiso.iso` affichait « disque endommagé », le `.iso` du 18/10 non | 2026-10-18 | 6a72f0528e (série s3) |
| Soulcalibur II (EU) | 🟠 Menus/intro | intro (720p), sauvegarde | 59,9 fps, intro 8,6 W et CPU 104 °C | 6889982e0a (720p/1080i à 60 Hz) obligatoire, sinon 50 fps comme l'amont | 2026-10-18 | 6a72f0528e (série s3) |
| XIII (USA, EU) | 🟠 Menus/intro | menu, création de profil | 59,9 fps, 2,6-3,5 W, CPU 59 °C | aucun défaut vu | 2026-10-18 | 6a72f0528e (série s3) |
| 007 Everything or Nothing, Capcom Classics 1 et 2, Castlevania Curse of Darkness, Riddick Butcher Bay, Fable, FIFA Street 2, Harry Potter Quidditch, Headhunter Redemption, Hulk, Tomb Raider Legend, Legend of Spyro, Mafia, Max Payne, Midnight Club 3, MotoGP 2, Ninja Gaiden Black, PES5, Psychonauts, Sonic Heroes, Street Fighter Anniversary, Top Spin | ⚪ Non testé | | | sur la carte, jamais lancés | | |

La série s3 a tourné sur un build local repris tel quel dans 6a72f0528e ; seul MotoGP a été remesuré sur l'APK exacte.
