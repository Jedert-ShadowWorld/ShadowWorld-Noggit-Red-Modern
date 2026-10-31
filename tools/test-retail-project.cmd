@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "SDK=C:\BuildTools\WindowsKits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%INCLUDE%"
set "LIB=%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64;%LIB%"
set "ROOT=I:\WOW\Server\Codex\Noggit"
set "BUILD=C:\NoggitBuild\Shadow-Noggit-Recovered"
set "OUT=%BUILD%\retail-project-test"
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"
cl /nologo /std:c++20 /EHsc /MT /I"%ROOT%\src\external\blizzard-archive-library\include" "%ROOT%\tests\retail_project_smoke.cpp" /Fe:retail-project-smoke.exe /link "%BUILD%\Release\blizzard-archive-library.lib" "%BUILD%\_deps\casclib-build\Release\casc.lib" "C:\NoggitBuild\Shadow-Noggit\_deps\stormlib-src\lib\release\x64\StormLibRAS.lib" wininet.lib advapi32.lib user32.lib
if errorlevel 1 exit /b 1
retail-project-smoke.exe "I:/WOW/Game/World of Warcraft" "D:/Server and Map Tools/Retail"
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
