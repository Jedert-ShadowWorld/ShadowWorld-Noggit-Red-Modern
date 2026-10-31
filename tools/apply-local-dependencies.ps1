$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
foreach ($entry in @(
    @('src/external/blizzard-archive-library', 'patches/blizzard-archive-local.patch'),
    @('dist/definitions', 'patches/definitions-retail-1215.patch'),
    @('cmake', 'patches/cmake-release.patch'))) {
    $directory = Join-Path $repo $entry[0]
    $patch = Join-Path $repo $entry[1]
    & git -C $directory apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -eq 0) { continue }
    & git -C $directory apply --check $patch
    if ($LASTEXITCODE -ne 0) { throw "Dependency patch conflicts: $directory" }
    & git -C $directory apply $patch
    if ($LASTEXITCODE -ne 0) { throw "Dependency patch failed: $directory" }
}
