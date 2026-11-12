@echo off
setlocal
cd /d "%~dp0"
echo VOLTOL Monitor - first install on ESP32-S3 4.0" screen board (ES3C40P, 16 MB)
echo This ERASES the board (settings, logs). For updates use UPDATE_S3_40_WINDOWS.bat
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 (
 py -3.11 -m pip install esptool==4.5.1
 if errorlevel 1 goto failed
)
set /p VCOM=Enter ESP32-S3 COM port (e.g. COM7): 
if not defined VCOM exit /b 1
py -3.11 -m esptool --chip esp32s3 --port "%VCOM%" --baud 460800 erase_flash
if errorlevel 1 goto failed
py -3.11 -m esptool --chip esp32s3 --port "%VCOM%" --baud 460800 write_flash 0x0 firmware\VOLTOL_S3_40_INSTALL.bin
if errorlevel 1 goto failed
echo SUCCESS. Setup - Internet Wi-Fi on the screen joins your Wi-Fi.
pause
exit /b 0
:failed
echo FAILED. Close any serial monitor. Check COM port / USB cable. Hold BOOT and tap RST if needed.
pause
exit /b 1
