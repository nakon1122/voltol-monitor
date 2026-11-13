@echo off
setlocal
cd /d "%~dp0"
echo VOTOL Monitor - record the start-up messages of the board (40 seconds)
echo Plug in BOTH USB-C ports of the P4 board if you can (two COM numbers in Device Manager).
set /p VCOM=Enter COM port(s), separated by a space (e.g. COM26 COM25): 
if not defined VCOM exit /b 1
echo The board restarts by itself now. If nothing appears, press RST once.
py -3.11 tools\serial_log.py %VCOM%
echo.
echo Send the file p4_log.txt from this folder.
pause
