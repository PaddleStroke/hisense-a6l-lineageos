@echo off
title A6L current build status
wsl.exe -d Ubuntu-24.04 -u root -- python3 /mnt/c/Users/Pierre/Desktop/A6L/tools/Show-RomBuildStatus.py --watch
if errorlevel 1 pause
