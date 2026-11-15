@echo off

setlocal

set "app=Oric.exe"

taskkill /f /im %app%

mingw32-make.exe

start /B %app%
