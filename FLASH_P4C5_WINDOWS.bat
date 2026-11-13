@echo off
setlocal
cd /d "%~dp0"
echo VOTOL Monitor - first install on KSDIY ESP32-P4C5 screen board (TEST build)
echo This ERASES the board (settings, logs).
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m pip install -q "esptool>=4.8.1,<5"
if errorlevel 1 goto failed
set /p VCOM=Enter ESP32-P4 COM port (the USB-C marked UART/USB, e.g. COM7): 
if not defined VCOM exit /b 1
py -3.11 -m esptool --chip esp32p4 --port "%VCOM%" --baud 460800 erase_flash
if errorlevel 1 goto failed
py -3.11 -m esptool --chip esp32p4 --port "%VCOM%" --baud 460800 write_flash 0x0 firmware\VOTOL_P4C5_INSTALL.bin
if errorlevel 1 goto failed
echo SUCCESS. Press RST on the board.
pause
exit /b 0
:failed
echo FAILED. Close any serial monitor. Check COM port / USB cable. Hold BOOT and tap RST if needed.
pause
exit /b 1
