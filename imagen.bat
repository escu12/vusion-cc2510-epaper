@echo off
rem Convierte y envia una imagen a la etiqueta: imagen doom.png  /  imagen doom.png -SoloVista
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0imagen.ps1" %*
