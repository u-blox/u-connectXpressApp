@echo off
REM Allows inbound TCP on private networks for the hardware tests (fake NTRIP caster in Python, ucxapp web/bridges).
REM Run as administrator. Remove again with: scripts\firewall-allow.cmd remove
net session >nul 2>&1 || (echo Run this from an administrator prompt. & exit /b 1)
set "APP=%~dp0..\app\bin\ucxapp.exe"
netsh advfirewall firewall delete rule name="ucxapp tests" >nul 2>&1
if /i "%~1"=="remove" (echo removed & exit /b 0)
netsh advfirewall firewall add rule name="ucxapp tests" dir=in action=allow profile=private protocol=TCP program="%APP%" >nul
for /f "delims=" %%P in ('where python 2^>nul') do netsh advfirewall firewall add rule name="ucxapp tests" dir=in action=allow profile=private protocol=TCP program="%%P" >nul
for /f "delims=" %%P in ('py -3 -c "import sys;print(sys.executable)" 2^>nul') do netsh advfirewall firewall add rule name="ucxapp tests" dir=in action=allow profile=private protocol=TCP program="%%P" >nul
if exist "%~dp0..\.venv\Scripts\python.exe" netsh advfirewall firewall add rule name="ucxapp tests" dir=in action=allow profile=private protocol=TCP program="%~dp0..\.venv\Scripts\python.exe" >nul
netsh advfirewall firewall show rule name="ucxapp tests" | findstr /i "Program Profiles"
for /f "tokens=*" %%N in ('powershell -NoProfile -Command "(Get-NetConnectionProfile).NetworkCategory"') do echo Network category: %%N (must be Private for these rules)
