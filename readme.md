### Émulateur Oric

Léger et rapide (en C gdi ou directx).
N'est pas fait pour émuler 100% de l'Oric. Mais peut lancer la majorité des jeux et des programmes.
Les démos marchent aussi (il y a peu de bidouille hardware sur Oric)

Il y a un accélération K7 avec démarrage automatique (bien pratique pour tester des jeux vite fait)
À lancer via l'explorateur ou faire un glisser déposer sur la fenêtre.

Possibilité de changer le clavier Qwerty ou Azerty. Certain jeux utilise la touche physique d'autre logiciel.

Sur Oric les touches de jeux c'est folkore. Donc possibilité de redéfinir les touches pour certains jeux (config enregistré dans le dossier du jeu)

Il y aussi un accélération du CPU. Soit "SYNC" qui respecte l'interruption. Donc la musique, le clavier restent synchro (sous basic par exemple ou les jeux qui se régle sur les IRQ).
L'autre c'est accélération total de l'Oric. Pratique pour accélérer les parties un peu longues des jeux (il y a en tellement...)

# todo

toggle pause
redéfinition des touches
utilisation de la manette + configuration par jeu/global
afficher un message en bas de la fenêtre lors d'un changement speed/snapshot/filter etc
reprendre le focus quand on glisse un fichier (chiant)
touche pour redimensionner x1 x2 x3 x4
toogle 4:3 1:1
screenshots
snapshot load/save

# done

rajouter plusieurs type de lissage eagle etc...
turbo qui conserve la vitesse des interruptions d'origine
dx11 (shaders ?) moins de cpu
redimensionnement de la fenêtre réglé sur la taille
menu clic droit pour les presets de taille ou les toogles fullscreen/scanline/lissage

# corrigé

les dsk marchent pas
les textes doubles sont inversés (ligne paire pas impaire)
les effets ça bouffe le CPU parce que c'est pas matériel donc dx+shaders

# hotkey

F1 Aide (messagebox simple)
F2 Explorateur de fichiers

XX CRT
XX Filters
XX Rembobinner la K7

XX Turbo sync
XX Turbo

XX stats

XX Soft reset
XX Hard reset

# roms (il y a un menu qui affiche les roms du dossier roms)

atmos & oric1 : rom originelles

slowboot : l'oric vérifie la mémoire au démarrage (lent)
fastboot : vérification de la mémoire désactivé. démarrage immédiat de l'oric
qwerty/azerty : agencement clavier uniquement sous basic/système.
sign/number : première rangée en azerty chiffre ou signes (comme sur un PC)
accents : la police de l'oric contient des accents comme sur le téléstrat. identique à l'amstrad cpc
*** attention certaine rom casse la compatibilité avec les jeux ***

# tapes

Jeux pour tester l'émulateur.

# Usage

Lancer le fichier Oric.exe (rien d'autre à installer)
Tous les réglages sont enregistré dans le fichiers settings.ini.

Par défaut l'émulateur démarre sur l'explorateur de fichier.
Si une rom est selectionnée elle sera par défaut pour les prochains démarrages.
Si c'est une K7 ou un Disquette elle démarre automatiquement.
