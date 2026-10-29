param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\rendering\WMOGroupRender.cpp'))
$header = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\rendering\WMOGroupRender.hpp'))
$start = $source.IndexOf('void WMOGroupRender::upload()')
$end = $source.IndexOf('void WMOGroupRender::initRenderBatches()', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'WMO renderer extraction markers missing' }
$structStart = $header.IndexOf('  struct WMORenderBatch')
$structEnd = $header.IndexOf('  class WMOGroupRender', $structStart)
if ($structStart -lt 0 -or $structEnd -lt 0) { throw 'WMO batch layout extraction markers missing' }
$fixture = [IO.File]::ReadAllText((Join-Path $root 'tests\wmo-render-regression.inc'))
$fixture = $fixture.Replace('// ACTUAL_BATCH_LAYOUT', $header.Substring($structStart, $structEnd-$structStart))
$fixture = $fixture.Replace('// ACTUAL_RENDER_FUNCTIONS', $source.Substring($start, $end-$start))
$textures = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\TextureManager.cpp'))
$textureStart = $textures.IndexOf('void TextureManager::unload_all(')
$textureEnd = $textures.IndexOf('TexArrayParams& TextureManager::get_tex_array(GLint compression', $textureStart)
if ($textureStart -lt 0 -or $textureEnd -lt 0) { throw 'Texture lifecycle extraction markers missing' }
$fixture = $fixture.Replace('// ACTUAL_TEXTURE_LIFECYCLE', $textures.Substring($textureStart, $textureEnd-$textureStart))
[IO.File]::WriteAllText($OutputPath, $fixture)
