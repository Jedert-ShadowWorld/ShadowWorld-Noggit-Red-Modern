param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\tools\MinimapTool.cpp'))
function Slice([string]$start, [string]$end) {
  $a = $source.IndexOf($start, [StringComparison]::Ordinal)
  if ($a -lt 0) { throw "Terrain export extraction marker missing: $start" }
  $b = $source.IndexOf($end, $a, [StringComparison]::Ordinal)
  if ($b -lt 0) { throw "Terrain export extraction marker missing: $end" }
  $source.Substring($a, $b - $a)
}
$fixture = [IO.File]::ReadAllText((Join-Path $root 'tests\terrain-export-regression.inc'))
$fixture = $fixture.Replace('// ACTUAL_EXPORT_FRAME', (Slice '    void MinimapTool::setExportActive(' '    void MinimapTool::writeMapTexturePair('))
$fixture = $fixture.Replace('// ACTUAL_EXPORT_FINISH', (Slice '    void MinimapTool::finishSaving(' '    void MinimapTool::saveSettings('))
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\MapView.cpp'))
$fixture = $fixture.Replace('// ACTUAL_FRAME_DISPATCH', (Slice '  auto* frameTool = activeTool().get();' '  if (!frameTool->preRender())'))
[IO.File]::WriteAllText($OutputPath, $fixture)
