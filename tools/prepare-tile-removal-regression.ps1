param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\world_tile_update_queue.cpp'))
$start = $source.IndexOf('  struct instance_update')
$end = $source.IndexOf('  world_tile_update_queue::world_tile_update_queue(', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Tile removal extraction markers missing' }
$fixture = [IO.File]::ReadAllText((Join-Path $root 'tests\tile-removal-regression.inc'))
$fixture = $fixture.Replace('// ACTUAL_UPDATE', $source.Substring($start, $end-$start))
$storage = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\world_model_instances_storage.cpp'))
$start = $storage.IndexOf('  void world_model_instances_storage::delete_instance(')
$end = $storage.IndexOf('  void world_model_instances_storage::unload_instance_', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Instance deletion extraction markers missing' }
$fixture = $fixture.Replace('// ACTUAL_DELETION', $storage.Substring($start, $end-$start))
[IO.File]::WriteAllText($OutputPath, $fixture)
