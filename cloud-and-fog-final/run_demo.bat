@echo off
REM ============================================================
REM  run_demo.bat  -  Full no-hardware demo
REM  Starts the synthetic data simulator AND the dashboard,
REM  each in its own window, then opens the dashboard in your
REM  browser. Close either window (or Ctrl-C in it) to stop.
REM ============================================================

cd /d "%~dp0"

REM ---- Path to your Python. Edit this one line if it ever changes. ----
set "PYTHON=c:\Users\User_11\Desktop\venv\Scripts\python.exe"

echo.
echo  Starting mesh demo (simulator + dashboard)...
echo  Two console windows will open. Close them to stop.
echo.

REM Feed synthetic data into MySQL
start "Mesh Simulator" cmd /k ""%PYTHON%" simulate_nodes.py"

REM Give the simulator a moment to connect/seed some data
timeout /t 2 /nobreak >nul

REM Launch the dashboard server
start "Mesh Dashboard" cmd /k ""%PYTHON%" dashboard\app.py"

REM Wait for the server to come up, then open the browser
timeout /t 3 /nobreak >nul
start "" http://localhost:5000

echo  Dashboard: http://localhost:5000
