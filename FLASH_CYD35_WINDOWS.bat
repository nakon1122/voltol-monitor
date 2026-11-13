@echo off
setlocal
cd /d "%~dp0"
echo VOTOL Monitor - install on ESP32-3248S035 (CYD 3.5 inch) - since 1.9.0 a full install is needed once
echo This ERASES the board (settings, logs).
py -3.11 --version
if errorlevel 1 goto nopy
py -3.11 -m pip install -q --disable-pip-version-check "esptool>=4.8.1,<5"
if errorlevel 1 goto failed
if not exist "firmware\VOTOL_CYD35_INSTALL.bin" goto nofile
set /p VCOM=Enter COM port (e.g. COM7): 
if not defined VCOM exit /b 1
rem erase + write in ONE connection: no reset in between (the chip's own USB port drops on reset)
set JOB=write_flash --erase-all 0x0 "firmware\VOTOL_CYD35_INSTALL.bin"

echo.
echo [1/3] Standard reset into download mode...
py -3.11 -m esptool --chip esp32 --port "%VCOM%" --baud 460800 --before default_reset --after hard_reset --connect-attempts 2 %JOB%
if not errorlevel 1 goto ok

echo [2/3] USB reset into download mode...
py -3.11 -m esptool --chip esp32 --port "%VCOM%" --baud 460800 --before usb_reset --after hard_reset --connect-attempts 2 %JOB%
if not errorlevel 1 goto ok

echo.
echo.
echo ===================================================================
echo [3/3] Put the board in download mode by hand:
echo    1. Unplug the USB cable
echo    2. Hold the BOOT button
echo    3. Plug the USB cable back in (keep holding BOOT)
echo    4. Release BOOT  - the screen stays dark, that is normal
echo    5. Check the COM number in Device Manager (it can change)
echo ===================================================================
set /p VCOM=Enter the COM port (Enter = %VCOM%): 
py -3.11 -m esptool --chip esp32 --port "%VCOM%" --baud 460800 --before no_reset --after hard_reset %JOB%
if errorlevel 1 goto failed

:ok
echo.
echo SUCCESS. Touch the 4 corner arrows to calibrate the screen.
pause
exit /b 0
:nopy
echo Python 3.11 is not installed.
pause
exit /b 1
:nofile
echo firmware\VOTOL_CYD35_INSTALL.bin not found next to this file.
pause
exit /b 1
:failed
echo FAILED. Close Arduino IDE / serial monitors / browser flasher tabs and try another USB cable or port.
pause
exit /b 1
