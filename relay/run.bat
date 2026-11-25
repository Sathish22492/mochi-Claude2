@echo off
cd /d "%~dp0"
title Mochi relay
python relay.py
echo.
echo The relay stopped. Scroll up to see why.
pause
