param(
    [string]$Configuration = "Release",
    [string]$Destination = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path (Split-Path $PSScriptRoot -Parent) "NoggitRED_ProjectSelector_Test"
}

$source = Join-Path $PSScriptRoot "build\bin\$Configuration"
$executable = Join-Path $source "noggit.exe"
$launcher = Join-Path $PSScriptRoot "dist\Start_Project_Selector.cmd"

if (-not (Test-Path -LiteralPath $executable)) {
    throw "Noggit executable not found: $executable"
}

if (-not (Test-Path -LiteralPath $launcher)) {
    throw "Project selector launcher not found: $launcher"
}

New-Item -ItemType Directory -Path $Destination -Force | Out-Null

Get-ChildItem -LiteralPath $source | Where-Object {
    $_.Name -ne "log.txt" -and $_.Name -notlike "log.before-*.txt"
} | Copy-Item -Destination $Destination -Recurse -Force

Copy-Item -LiteralPath $launcher -Destination $Destination -Force

Write-Host "Project selector test package created at: $Destination"
Write-Host "Start it with: Start_Project_Selector.cmd"
