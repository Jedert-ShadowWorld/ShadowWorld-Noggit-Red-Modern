param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\rendering\ModelRender.cpp'))
$start = $source.IndexOf('void ModelRender::upload()')
$end = $source.IndexOf('void ModelRender::draw(', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'M2 renderer lifecycle extraction markers missing' }
$fixture = [IO.File]::ReadAllText((Join-Path $root 'tests\m2-lifecycle-regression.inc'))
$fixture = $fixture.Replace('// ACTUAL_LIFECYCLE', $source.Substring($start, $end-$start))
$model = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\Model.cpp'))
$resetStart = $model.IndexOf('  _textureFilenames.', $model.IndexOf('  // textures'))
$resetEnd = $model.IndexOf('  for (size_t i = 0; i < header.nTextures;', $resetStart)
if ($resetStart -lt 0 -or $resetEnd -lt 0) { throw 'M2 texture key reset markers missing' }
$fixture = $fixture.Replace('// ACTUAL_KEY_RESET', $model.Substring($resetStart, $resetEnd-$resetStart))
[IO.File]::WriteAllText($OutputPath, $fixture)
