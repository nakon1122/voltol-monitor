@echo off
setlocal
cd /d "%~dp0"
echo VOTOL Monitor - record the start-up messages of the P4 board (for checking a blank screen)
set /p VCOM=Enter ESP32-P4 COM port (e.g. COM25): 
if not defined VCOM exit /b 1
echo.
echo Recording for 30 seconds. Press RST on the board NOW.
py -3.11 -c "import serial,time,sys;p=serial.Serial(sys.argv[1],115200,timeout=0.2);p.dtr=False;p.rts=False;f=open('p4_log.txt','wb');t=time.time();exec('while time.time()-t<30:\n d=p.read(4096)\n if d: f.write(d); sys.stdout.write(d.decode(\'ascii\',\'replace\')); sys.stdout.flush()');f.close()" %VCOM%
echo.
echo Saved: p4_log.txt  (send this file)
pause
