@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "SDK=C:\BuildTools\WindowsKits\10"
set "SDKVER=10.0.26100.0"
set "INCLUDE=%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%INCLUDE%"
set "LIB=%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64;%LIB%"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "OUT=C:\NoggitBuild\RecoveryTests"
pushd "%OUT%"
cl /nologo /EHsc /std:c++20 /MT /DNOMINMAX /I"%ROOT%\src\external\blizzard-database-library\include" "%ROOT%\tests\retail_db2_smoke.cpp" /Fe:retail-db2-smoke.exe /link "C:\NoggitBuild\Shadow-Noggit-Recovered\Release\blizzard-database-library.lib"
if errorlevel 1 exit /b 1
set "FAILED=0"
for %%T in (AreaTable AreaTrigger Map LoadingScreens Light LightData LightParams LightSkybox GroundEffectDoodad GroundEffectTexture TerrainType LiquidType LiquidMaterial SoundProviderPreferences SoundAmbience ZoneMusic ZoneIntroMusicTable WMOAreaTable SoundKit WindSettings Weather WeatherXParticulate ZoneLight ZoneLightPoint MapDifficulty) do (
  retail-db2-smoke.exe "%ROOT%\dist\definitions" "D:\Server and Map Tools\Retail\dbfilesclient" %%T
  if errorlevel 1 set "FAILED=1"
)
popd
exit /b %FAILED%
