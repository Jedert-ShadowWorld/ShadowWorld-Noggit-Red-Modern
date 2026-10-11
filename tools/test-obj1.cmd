@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "SDK=C:\BuildTools\WindowsKits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%INCLUDE%"
set "LIB=%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64;%LIB%"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
if not exist "C:\NoggitBuild\RecoveryTests" mkdir "C:\NoggitBuild\RecoveryTests"
cd /d "C:\NoggitBuild\RecoveryTests"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\prepare-obj1-regression.ps1" -OutputPath "%CD%\obj1-regression.cpp"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++20 /DNOMINMAX /I"%ROOT%\src" /I"%ROOT%\src\external" obj1-regression.cpp /Fe:obj1-regression.exe
if errorlevel 1 exit /b 1
obj1-regression.exe %*
exit /b %errorlevel%
