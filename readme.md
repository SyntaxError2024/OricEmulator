### Émulateur Oric

Léger et rapide (en C, avec GDI ou DirectX).
N'est pas conçu pour émuler 100 % de l'Oric, mais il peut lancer la majorité des jeux et des programmes.
Les démos fonctionnent aussi (il y a peu de bidouillage matériel sur l'Oric).

Il existe une accélération K7 avec démarrage automatique (pratique pour tester des jeux rapidement).
Peut lancer des jeux via l'explorateur de fichiers avec un glisser-déposer sur la fenêtre.

Possibilité de changer le clavier QWERTY ou AZERTY. Certains jeux utilisent la touche physique d'un autre logiciel.

# Compilation

Installer mingw64 [lien](https://github.com/niXman/mingw-builds-binaries/releases) (compilateur C open source).
Lancer le fichier build.bat.
Il va compiler l'émulateur et le lancer.
Ou alors lancer directement le fichier Oric.exe précompilé.
(Pourquoi c'est aussi compliqué de trouver ce foutu compilateur sur le net ?!?! Il y a 3000 versions)

# À faire / TODO

Sur Oric, les touches de jeu sont parfois folkloriques. Il est donc possible de redéfinir les touches pour certains jeux (configuration enregistrée dans le dossier du jeu).

redéfinition des touches
utilisation de la manette + configuration par jeu/global
afficher un message en bas de la fenêtre lors d'un changement de vitesse/capture/filtre, etc.
reprendre le focus quand on glisse un fichier (bug)
touche pour redimensionner x1 x2 x3 x4
toggle 4:3 / 1:1

# Fonctionnalités

shader pour émuler un écran CRT et plusieurs types de lissage (le meilleur étant le dernier)
turbo CPU qui conserve la vitesse des interruptions d'origine
DirectX 11 ou GDI (léger, mais consomme du CPU pour les filtres)

# Raccourcis

# ROMs (il y a un menu qui affiche les ROM du dossier roms)

atmos & oric-1 : ROM d'origine

slowboot : l'Oric vérifie la mémoire au démarrage (lent)
fastboot : vérification de la mémoire désactivée ; démarrage immédiat de l'Oric
qwerty/azerty : agencement du clavier uniquement sous Basic/système
sign/number : première rangée en AZERTY, chiffres ou signes (comme sur un PC)
accents : la police de l'Oric contient des accents comme sur le téléstrat, identique à l'Amstrad CPC
*** attention, certaines ROM cassent la compatibilité avec les jeux ***

# tapes/ et dsk/

Jeux pour tester l'émulateur.

# Utilisation

Lancer le fichier Oric.exe (rien d'autre à installer).
Tous les réglages sont enregistrés dans le fichier settings.ini.

Par défaut, l'émulateur démarre sur l'explorateur de fichiers (ESC pour le quitter).
Si une ROM est sélectionnée, elle sera utilisée par défaut pour les prochains démarrages.
Si c'est une K7 ou une disquette, elle démarre automatiquement.
