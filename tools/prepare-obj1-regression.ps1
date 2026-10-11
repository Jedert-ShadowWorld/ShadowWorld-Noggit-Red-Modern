param([string]$OutputPath = (Join-Path $env:TEMP 'noggit-obj1-regression.cpp'))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = [IO.File]::ReadAllText((Join-Path $root 'src\noggit\formats\adt\ShadowlandsADTCodec.cpp'))
function Slice([string]$start, [string]$end) {
  $a = $source.IndexOf($start)
  $b = $source.IndexOf($end, $a)
  if ($a -lt 0 -or $b -lt 0) { throw 'Source extraction marker missing' }
  $source.Substring($a, $b - $a)
}
$includes = @'
#include <noggit/MapHeaders.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
'@
$mock = @'
namespace BlizzardArchive { enum class ClientVersion { WOTLK, SL, RETAIL }; }
struct TestListfile {
  unsigned int getFileDataID(std::string const& text) { return static_cast<unsigned int>(std::stoul(text)); }
};
struct TestClient {
  TestListfile entries;
  TestListfile* listfile() { return &entries; }
  BlizzardArchive::ClientVersion version() { return BlizzardArchive::ClientVersion::RETAIL; }
};
namespace Noggit::Application {
  struct NoggitApplication {
    TestClient client;
    static NoggitApplication* instance() { static NoggitApplication app; return &app; }
    TestClient* clientData() { return &client; }
  };
}
'@
$code = $includes + "`n" + $mock + "`n" + (Slice '    struct ChunkView' '    constexpr std::size_t part_index') + (Slice '    std::vector<ChunkView> parse_chunks' '    std::vector<std::uint8_t> chunk_bytes') + (Slice '    std::vector<std::uint8_t> replace_top_level_chunk' '    void validate_tex0_alpha_layout') + (Slice '    std::string indexed_path' '    void save_parts') + [IO.File]::ReadAllText((Join-Path $root 'tests\obj1-regression-main.inc'))
[IO.File]::WriteAllText($OutputPath, $code)
