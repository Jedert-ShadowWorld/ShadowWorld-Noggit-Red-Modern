param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\formats\adt\ShadowlandsADTCodec.cpp'))
function Slice([string]$start, [string]$end) {
  $a = $source.IndexOf($start, [StringComparison]::Ordinal)
  if ($a -lt 0) { throw "LOD reference extraction marker missing: $start" }
  $b = $source.IndexOf($end, $a, [StringComparison]::Ordinal)
  if ($b -lt 0) { throw "LOD reference extraction marker missing: $end" }
  $source.Substring($a, $b - $a)
}
$fixture = [IO.File]::ReadAllText((Join-Path $root 'tests\lod-reference-regression.inc'))
$fixture = $fixture.Replace('// ACTUAL_PART_INDEX', (Slice '    constexpr std::size_t part_index' '    std::string split_path'))
$fixture = $fixture.Replace('// ACTUAL_REFERENCE_HELPERS', (Slice '    using LodBytes =' '    bool update_lod('))
$updateLod = Slice '    bool update_lod(' '  bool ShadowlandsADTBackingStore::has'
if ($updateLod -notmatch '\r?\n  \}\s*$') { throw 'LOD update extraction lost its anonymous namespace boundary.' }
$fixture = $fixture.Replace('// ACTUAL_UPDATE_LOD', ($updateLod -replace '\r?\n  \}\s*$', ''))
$fixture = $fixture.Replace('// ACTUAL_BACKING_HAS', (Slice '  bool ShadowlandsADTBackingStore::has' '  std::string ShadowlandsADTCodec::lodReferenceDirectory'))
$fixture = $fixture.Replace('// ACTUAL_BACKING_PART', (Slice '  ShadowlandsADTRawPart const& ShadowlandsADTBackingStore::part' '  ShadowlandsADTSource ShadowlandsADTCodec::makeSource'))
$fixture = $fixture.Replace('// ACTUAL_REFERENCE_CONFIGURATION', (Slice '  std::string ShadowlandsADTCodec::lodReferenceDirectory' '  void ShadowlandsADTCodec::rebuildDerived'))
[IO.File]::WriteAllText($OutputPath, $fixture)
