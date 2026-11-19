@echo off

setlocal

set "app=Oric.exe"

rem tuer l'émulateur s'il est déjà en cours d'exécution

taskkill /f /im %app%

rem lancer la compilation du projet

mingw32-make.exe

rem lancer l'émulateur en arrière-plan

start /B %app%
