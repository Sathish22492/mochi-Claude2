@echo off
setlocal
cd /d "%~dp0"
title Mochi relay setup
echo ==============================
echo   Mochi relay - one time setup
echo ==============================
echo.

python --version >nul 2>nul
if errorlevel 1 goto nopython

echo [1/4] Installing Python libraries. This can take a few minutes...
python -m pip install --upgrade pip >nul 2>nul
python -m pip install -r requirements.txt
if errorlevel 1 goto piperr

if exist "tools\piper\piper.exe" goto havepiper
echo.
echo [2/4] Downloading the Piper voice engine...
if not exist tools mkdir tools
curl -fL -o tools\piper.zip https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip
if errorlevel 1 goto dlfail
tar -xf tools\piper.zip -C tools
del tools\piper.zip
if not exist "tools\piper\piper.exe" goto dlfail
:havepiper

if exist "tools\voices\en_US-lessac-medium.onnx.json" goto havevoice
echo.
echo [2/4] Downloading the English voice...
if not exist tools\voices mkdir tools\voices
curl -fL -o tools\voices\en_US-lessac-medium.onnx https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx
if errorlevel 1 goto dlfail
curl -fL -o tools\voices\en_US-lessac-medium.onnx.json https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
if errorlevel 1 goto dlfail
:havevoice

if exist ".env" goto haveenv
echo.
echo [3/4] Now your FREE Groq key.
echo   1. Open https://console.groq.com/keys and sign up
echo   2. Click Create API Key and copy it
echo.
set /p GROQKEY=Paste the key here and press Enter: 
if "%GROQKEY%"=="" goto nokey
(
echo LLM_PROVIDER=groq
echo GROQ_API_KEY=%GROQKEY%
echo PIPER_BIN=%~dp0tools\piper\piper.exe
echo PIPER_VOICE=%~dp0tools\voices\en_US-lessac-medium.onnx
echo WHISPER_MODEL=base.en
echo WHISPER_LANG=en
echo DEVICE_VOLUME=70
echo FOLLOWUP_WINDOW=8
echo MIN_RMS=400
echo DEBUG_LEVELS=0
echo PERSONA_EXTRA=
) > .env
:haveenv

echo.
echo [4/4] Allowing the relay through the Windows firewall for home networks...
netsh advfirewall firewall add rule name="Mochi relay" dir=in action=allow protocol=TCP localport=8765 profile=private >nul 2>nul
if errorlevel 1 echo   Could not add the rule without admin rights. If the device cannot connect, right click setup.bat and choose Run as administrator once.

echo.
echo Setup done. Starting the relay. Keep this window open.
echo The first start downloads the speech model and takes a minute.
echo.
python relay.py
echo.
echo The relay stopped. Scroll up to see why.
pause
exit /b 0

:nopython
echo Python is not installed. Installing it now with winget...
winget install -e --id Python.Python.3.12 --accept-source-agreements --accept-package-agreements
echo.
echo Python install finished. CLOSE this window, then double click setup.bat again.
pause
exit /b 0

:nokey
echo No key entered. Run setup.bat again and paste your Groq key.
pause
exit /b 1

:dlfail
echo.
echo A download failed. Check your internet connection and run setup.bat again.
echo If it keeps failing, copy the red or error text and send it to Claude.
pause
exit /b 1

:piperr
echo.
echo Installing the Python libraries failed. Copy the error text above and send it to Claude.
pause
exit /b 1
