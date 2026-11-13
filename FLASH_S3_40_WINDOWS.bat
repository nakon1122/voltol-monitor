@echo off
setlocal
cd /d "%~dp0"
echo VOTOL Monitor - first install on ESP32-S3 4.0" screen board (ES3C40P, 16 MB)
echo This ERASES the board (settings, logs).
py -3.11 --version
if errorlevel 1 goto failed
py -3.11 -m pip install -q --disable-pip-version-check "esptool>=4.8.1,<5"
if errorlevel 1 goto failed
set /p VCOM=Enter COM port (e.g. COM7): 
if not defined VCOM exit /b 1
rem erase + write in ONE connection: no reset in between (USB boards drop the port on reset)
set JOB=write_flash --erase-all 0x0 firmware\VOTOL_S3_40_INSTALL.bin

echo.
echo [1/3] Automatic download mode...
py -3.11 -m esptool --chip esp32s3 --port "%VCOM%" --baud 460800 --before default_reset --connect-attempts 3 %JOB%
if not errorlevel 1 goto ok

echo.
echo [2/3] USB reset of the chip's own USB port...
py -3.11 -m esptool --chip esp32s3 --port "%VCOM%" --baud 460800 --before usb_reset --connect-attempts 3 %JOB%
if not errorlevel 1 goto ok

echo.
echo [3/3] Put the board in download mode by hand:
echo    1. Hold BOOT
echo    2. Press and release RST (or unplug and plug the USB cable) while holding BOOT
echo    3. Release BOOT
echo    (the COM number can change - check Device Manager)
set /p VCOM=Enter the COM port again (Enter = %VCOM%): 
py -3.11 -m esptool --chip esp32s3 --port "%VCOM%" --baud 115200 --before no_reset %JOB%
if errorlevel 1 goto failed

:ok
echo SUCCESS. Press RST if the screen stays dark.
pause
exit /b 0
:failed
echo FAILED. Close Arduino IDE / serial monitors / the web flasher tab, unplug and replug the USB cable.
pause
exit /b 1
