@echo off
rem Compila y graba un firmware en el CC2510: grabar receptor  /  grabar puente -Borrar
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0grabar.ps1" %*
