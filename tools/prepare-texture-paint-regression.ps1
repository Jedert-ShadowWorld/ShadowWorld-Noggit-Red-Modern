param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\texture_set.cpp'))
function Read-Section([string]$begin, [string]$end) {
  $start = $source.IndexOf($begin)
  $stop = $source.IndexOf($end, $start)
  if ($start -lt 0 -or $stop -le $start) { throw "Texture paint extraction marker missing: $begin" }
  return $source.Substring($start, $stop - $start)
}
$helper = Read-Section 'namespace' 'TextureSet::TextureSet ('
$methods = Read-Section 'bool TextureSet::paintTexture(' ' size_t const& TextureSet::num()'
$fixture = [IO.File]::ReadAllText((Join-Path $root 'tests\texture-paint-regression.inc'))
if (!$fixture.Contains('// ACTUAL_PAINT_HELPER') -or !$fixture.Contains('// ACTUAL_PAINT_METHODS')) {
  throw 'Texture paint fixture markers missing'
}
[IO.File]::WriteAllText($OutputPath, $fixture.Replace('// ACTUAL_PAINT_HELPER', $helper).Replace('// ACTUAL_PAINT_METHODS', $methods))
