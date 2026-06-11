@echo off
REM ============================================================
REM  run_dashboard.bat  -  Dashboard only
REM  Starts just the dashboard server (no simulator). Use this
REM  when real data is already flowing into MySQL, e.g. from
REM  serial_to_mysql.py with the hardware plugged in.
REM ============================================================

cd /d "%~dp0"

REM ---- Path to your Python. Edit this one line if it ever changes. ----
set "PYTHON=c:\kitty\AppData\Local\Programs\Python\Python312\python.exe"

echo.
echo  Starting dashboard...
echo.

REM Open the browser shortly after the server starts
start "" /min cmd /c "timeout /t 3 /nobreak >nul & start "" http://localhost:5000"

echo  Dashboard: http://localhost:5000   (Ctrl-C to stop)
echo.

REM Run in this window so Ctrl-C stops it cleanly
"%PYTHON%" dashboard\app.py
