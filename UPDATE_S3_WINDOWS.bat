@echo off
setlocal
cd /d "%~dp0"
echo VOTOL Monitor - update ESP32-S3 (keeps settings, logs, backups)
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 (
 py -3.11 -m pip install esptool==4.5.1
 if errorlevel 1 goto failed
)
set /p VCOM=Enter ESP32-S3 COM port (e.g. COM7): 
if not defined VCOM exit /b 1
py -3.11 -m esptool --chip esp32s3 --port "%VCOM%" --baud 460800 write_flash 0xe000 firmware\BOOT_APP0.bin 0x10000 firmware\VOTOL_S3_APP_UPDATE.bin
if errorlevel 1 goto failed
echo SUCCESS.
pause
exit /b 0
:failed
echo FAILED. Close any serial monitor. Check COM port / USB cable. Hold BOOT and tap RST if needed.
pause
exit /b 1
