@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "SDK=C:\BuildTools\WindowsKits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%INCLUDE%"
set "LIB=%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64;%LIB%"
set "QT=D:\Qt\5.15.2\msvc2019_64"
set "PATH=%QT%\bin;%PATH%"
set "ROOT=I:\WOW\Server\Codex\Noggit"
if not exist "C:\NoggitBuild\RecoveryTests" mkdir "C:\NoggitBuild\RecoveryTests"
cd /d "C:\NoggitBuild\RecoveryTests"
cl /nologo /EHsc /std:c++20 /MD /DNOMINMAX /I"%ROOT%\src" /I"%QT%\include" /I"%QT%\include\QtCore" "%ROOT%\tests\listfile_update.cpp" /Fe:listfile-update-test.exe /link /LIBPATH:"%QT%\lib" Qt5Core.lib
if errorlevel 1 exit /b 1
listfile-update-test.exe
exit /b %errorlevel%
