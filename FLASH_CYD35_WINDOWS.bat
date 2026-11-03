@echo off
setlocal
cd /d "%~dp0"
echo VOLTOL Monitor - first install on ESP32-3248S035 (CYD 3.5 inch)
echo This ERASES the board (settings, logs). For updates use UPDATE_CYD35_WINDOWS.bat
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m esptool version >nul 2>nul
if errorlevel 1 (
 py -3.11 -m pip install esptool==4.5.1
 if errorlevel 1 goto failed
)
set /p VCOM=Enter display board COM port (e.g. COM6): 
if not defined VCOM exit /b 1
py -3.11 -m esptool --chip esp32 --port "%VCOM%" --baud 460800 erase_flash
if errorlevel 1 goto failed
py -3.11 -m esptool --chip esp32 --port "%VCOM%" --baud 460800 write_flash 0x0 firmware\VOLTOL_CYD35_INSTALL.bin
if errorlevel 1 goto failed
echo SUCCESS. Touch the 4 corner arrows to calibrate the screen.
pause
exit /b 0
:failed
echo FAILED. Close any serial monitor. Check COM port / USB cable (CH340 driver). Hold BOOT and tap RST if needed.
pause
exit /b 1
