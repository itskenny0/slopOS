@echo off
rem  Write a PicoOS image to a USB stick, byte for byte.
rem
rem  Right-click this file and choose "Run as administrator". Writing to a
rem  whole disk rather than to a file on one is not something Windows lets a
rem  normal program do, so without that it will stop and say so.
rem
rem  It will ask which stick and which image. Read what it says before you
rem  answer: everything on the stick you pick is going away.
rem
rem  If a stick has stopped taking writes -- Windows offering to format it,
rem  or any tool giving "write error" on every image -- run this instead:
rem
rem      writeusb.bat -Repair
rem
rem  That wipes it and hands it back as an ordinary empty disk, which is
rem  also the state it needs to be in before an image is written to it.

cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0writeusb.ps1" %*
echo.
pause
