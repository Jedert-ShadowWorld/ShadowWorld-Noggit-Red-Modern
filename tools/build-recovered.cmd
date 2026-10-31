@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "SDK=C:\BuildTools\WindowsKits\10"
set "SDKVER=10.0.26100.0"
set "PATH=%SDK%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%INCLUDE%"
set "LIB=%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64;%LIB%"
set "ROOT=I:\WOW\Server\Codex\Noggit"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\apply-local-dependencies.ps1"
if errorlevel 1 exit /b 1
set "BUILD=C:\NoggitBuild\Shadow-Noggit-Recovered"
set "DEPS=C:\NoggitBuild\Shadow-Noggit\_deps"
set "CMAKE=I:\WOW\Server\Codex\BuildTools\cmake-4.3.4-windows-x86_64\bin\cmake.exe"
set "NINJA=I:\WOW\Server\Codex\BuildTools\ninja-1.13.1\ninja.exe"
if not "%~1"=="build" (
  "%CMAKE%" -S "%ROOT%" -B "%BUILD%" -G "Ninja Multi-Config" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_PREFIX_PATH="D:\Qt\5.15.2\msvc2019_64" -DCMAKE_CXX_FLAGS_RELEASE="" -DCMAKE_C_FLAGS_RELEASE="" -DCMAKE_SYSTEM_VERSION=%SDKVER% -DFETCHCONTENT_SOURCE_DIR_CASCLIB="%DEPS%\casclib-src" -DFETCHCONTENT_SOURCE_DIR_FASTNOISE2="%DEPS%\fastnoise2-src" -DFETCHCONTENT_SOURCE_DIR_JSON="%DEPS%\json-src" -DFETCHCONTENT_SOURCE_DIR_LODEPNG="%DEPS%\lodepng-src" -DFETCHCONTENT_SOURCE_DIR_LUA="%DEPS%\lua-src" -DFETCHCONTENT_SOURCE_DIR_SOL2="%DEPS%\sol2-src" -DFETCHCONTENT_SOURCE_DIR_STORMLIB="%DEPS%\stormlib-src"
  if errorlevel 1 exit /b 1
)
if "%~1"=="configure" exit /b 0
"%NINJA%" -C "%BUILD%" -f build-Release.ninja bin/Release/noggit.exe -j 6
exit /b %errorlevel%
