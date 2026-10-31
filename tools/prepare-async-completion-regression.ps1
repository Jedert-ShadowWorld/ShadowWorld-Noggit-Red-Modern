param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\AsyncObject.cpp'))
$start = $source.IndexOf('void AsyncObject::complete_loading(')
$end = $source.IndexOf('AsyncObject::~AsyncObject()', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Async completion extraction markers missing' }
$fixture = [IO.File]::ReadAllText((Join-Path $root 'tests\async-completion-regression.inc'))
$fixture = $fixture.Replace('// ACTUAL_COMPLETION', $source.Substring($start, $end-$start))
[IO.File]::WriteAllText($OutputPath, $fixture)
