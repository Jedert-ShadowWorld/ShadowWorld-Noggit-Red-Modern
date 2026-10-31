param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\ui\ObjectEditor.cpp'))
$start = $source.IndexOf('    void object_editor::copy_current_selection(World* world)')
if ($start -lt 0) { throw 'Clipboard function marker missing' }
$end = $source.IndexOf('    void object_editor::SaveObjecttoTXT', $start)
if ($end -lt 0) { throw 'Clipboard end marker missing' }
$fixture = [IO.File]::ReadAllText((Join-Path $root 'tests\object-clipboard-regression.inc'))
$labelStart = $source.IndexOf('    void object_editor::update_clipboard()')
$labelEnd = $source.IndexOf('    void object_editor::copy(std::string const& filename)', $labelStart)
if ($labelStart -lt 0 -or $labelEnd -lt 0) { throw 'Clipboard label extraction marker missing' }
if (!$fixture.Contains('// ACTUAL_CLIPBOARD_FUNCTION')) { throw 'Clipboard fixture marker missing' }
[IO.File]::WriteAllText($OutputPath, $fixture.Replace('// ACTUAL_CLIPBOARD_FUNCTION', $source.Substring($start, $end - $start) + $source.Substring($labelStart, $labelEnd - $labelStart)))
