@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "SDK=C:\BuildTools\WindowsKits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%INCLUDE%"
set "LIB=%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64;%LIB%"
set "ROOT=I:\WOW\Server\Codex\Noggit"
set "OUT=C:\NoggitBuild\Shadow-Noggit-Recovered\custom-listfile-test"
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"
cl /nologo /std:c++20 /EHsc /MD /I"%ROOT%\src\external\blizzard-archive-library\include" "%ROOT%\tests\listfile_custom_overrides.cpp" "%ROOT%\src\external\blizzard-archive-library\src\Listfile.cpp" /Fe:listfile-custom-test.exe
if errorlevel 1 exit /b 1
listfile-custom-test.exe
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
