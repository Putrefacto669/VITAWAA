@echo off
chcp 65001 >nul
title VitaWA bridge
cd /d "%~dp0"
if not exist "%~dp0node_modules\baileys" (
  echo Falta instalar las dependencias. Lo hago una sola vez, espera un momento...
  call npm install --omit=dev
  if errorlevel 1 (
    echo No se pudo instalar. Mejor usa la carpeta VitaWA-Bridge-Windows que ya trae todo.
    pause
    exit /b 1
  )
)
set OPEN_BROWSER=1
if exist "%~dp0node\node.exe" (
  "%~dp0node\node.exe" server.mjs
) else (
  node server.mjs
)
echo.
echo El bridge se detuvo. Puedes cerrar esta ventana.
pause
