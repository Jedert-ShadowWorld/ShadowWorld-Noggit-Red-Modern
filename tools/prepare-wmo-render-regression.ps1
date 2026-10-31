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
$fixture = $fixture.Replace('// ACTUAL_BATCH_INIT', $source.Substring($end))
$wmoSource = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\WMO.cpp'))
$colorStart = $wmoSource.IndexOf('  glm::vec4 colorFromInt(')
$colorEnd = $wmoSource.IndexOf('void WMOGroup::load()', $colorStart)
$mocvStart = $wmoSource.IndexOf('void WMOGroup::load_mocv(')
$mocvEnd = $wmoSource.IndexOf('bool WMOGroup::is_visible(', $mocvStart)
if ($colorStart -lt 0 -or $colorEnd -lt 0 -or $mocvStart -lt 0 -or $mocvEnd -lt 0) { throw 'WMO lighting extraction markers missing' }
$colorFunction = 'namespace {' + $wmoSource.Substring($colorStart, $colorEnd-$colorStart).Replace('GLubyte', 'unsigned char')
$fixture = $fixture.Replace('Root root(unsigned seed)', $colorFunction + $wmoSource.Substring($mocvStart, $mocvEnd-$mocvStart).Replace('WMOGroup::', 'Group::') + 'Root root(unsigned seed)')
$textures = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\TextureManager.cpp'))
$textureStart = $textures.IndexOf('void TextureManager::unload_all(')
$textureEnd = $textures.IndexOf('#include <cstdint>', $textureStart)
if ($textureStart -lt 0 -or $textureEnd -lt 0) { throw 'Texture lifecycle extraction markers missing' }
$fixture = $fixture.Replace('// ACTUAL_TEXTURE_LIFECYCLE', $textures.Substring($textureStart, $textureEnd-$textureStart))
$uploadStart = $textures.IndexOf('void blp_texture::upload()')
$uploadEnd = $textures.IndexOf('void blp_texture::unload()', $uploadStart)
if ($uploadStart -lt 0 -or $uploadEnd -lt 0) { throw 'BLP upload extraction markers missing' }
$fixture = $fixture.Replace('// ACTUAL_BLP_UPLOAD', $textures.Substring($uploadStart, $uploadEnd-$uploadStart).Replace('blp_texture::upload', 'UploadTexture::upload'))
$guard = [IO.File]::ReadAllText((Join-Path $root 'src\opengl\ScopedTextureArrayBinding.hpp'))
$guardStart = $guard.IndexOf('namespace OpenGL::Scoped')
if ($guardStart -lt 0) { throw 'Texture binding guard extraction marker missing' }
$fixture = $fixture.Replace('// ACTUAL_TEXTURE_BINDING_GUARD', $guard.Substring($guardStart))
$textureHeader = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\TextureManager.h'))
$layoutStart = $textureHeader.IndexOf('struct TexArrayAllocation')
$layoutEnd = $textureHeader.IndexOf('class TextureManager', $layoutStart)
if ($layoutStart -lt 0 -or $layoutEnd -lt 0) { throw 'Texture array layout extraction markers missing' }
$fixture = $fixture.Replace('// ACTUAL_TEXTURE_ARRAY_LAYOUT', $textureHeader.Substring($layoutStart, $layoutEnd-$layoutStart))
$types = [IO.File]::ReadAllText((Join-Path $root 'src\opengl\types.hpp'))
$layoutStart = $types.IndexOf('  struct ChunkExtLayerParams')
$layoutEnd = $types.IndexOf('  struct LiquidChunkInstanceDataUniformBlock', $layoutStart)
if ($layoutStart -lt 0 -or $layoutEnd -lt 0) { throw 'Extended layer layout extraction markers missing' }
$fixture = $fixture.Replace('// ACTUAL_EXT_LAYER_LAYOUT', $types.Substring($layoutStart, $layoutEnd-$layoutStart))
$sets = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\texture_set.cpp'))
$lookupStart = $sets.IndexOf('blp_texture* TextureSet::heightTexture(')
$lookupEnd = $sets.IndexOf('void TextureSet::setHeightMappingData(', $lookupStart)
if ($lookupStart -lt 0 -or $lookupEnd -lt 0) { throw 'Height texture lookup extraction markers missing' }
$lookup = $sets.Substring($lookupStart, $lookupEnd-$lookupStart).Replace('blp_texture* TextureSet::heightTexture', 'Texture* TileTextureSet::heightTexture')
$fixture = $fixture.Replace('// ACTUAL_HEIGHT_TEXTURE_LOOKUP', $lookup)
$tileSource = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\rendering\TileRender.cpp'))
$tileHeader = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\rendering\TileRender.hpp'))
$pruneStart = $tileSource.IndexOf('void TileRender::pruneUnusedSamplers(')
$pruneEnd = $tileSource.IndexOf('void Noggit::Rendering::TileRender::setChunkGroundEffectColor(', $pruneStart)
$callStart = $tileHeader.IndexOf('  struct MapTileDrawCall')
$callEnd = $tileHeader.IndexOf('  class TileRender', $callStart)
if ($pruneStart -lt 0 -or $pruneEnd -lt 0 -or $callStart -lt 0 -or $callEnd -lt 0) { throw 'Tile sampler extraction markers missing' }
$fixture = $fixture.Replace('// ACTUAL_TILE_DRAW_CALL', $tileHeader.Substring($callStart, $callEnd-$callStart))
$fixture = $fixture.Replace('// ACTUAL_TILE_SAMPLER_PRUNING', $tileSource.Substring($pruneStart, $pruneEnd-$pruneStart))
$readiness = [regex]::Match($tileHeader, 'bool texturesReady\(\) const \{[^\r\n]+\}')
if (!$readiness.Success) { throw 'Tile readiness extraction marker missing' }
$fixture = $fixture.Replace('// ACTUAL_TILE_READINESS', $readiness.Value)
[IO.File]::WriteAllText($OutputPath, $fixture)
