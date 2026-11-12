@echo off
setlocal
cd /d "%~dp0"
echo VOTOL Monitor - first install on ESP32-S3 DevKitC-1 (8 MB, web dashboard)
echo This ERASES the board (settings, logs). For updates use UPDATE_S3_WINDOWS.bat
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
py -3.11 -m esptool --chip esp32s3 --port "%VCOM%" --baud 460800 write_flash 0x0 firmware\VOTOL_S3_INSTALL.bin
if errorlevel 1 goto failed
echo SUCCESS. Connect your phone to Wi-Fi VOTOL-xxxx (password votol1234) and open http://192.168.4.1
pause
exit /b 0
:failed
echo FAILED. Close any serial monitor. Check COM port / USB cable. Hold BOOT and tap RST if needed.
pause
exit /b 1
