// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <memory>

namespace BlizzardArchive
{
  class ClientData;
}

namespace Noggit::ClientData
{
  // Loads Shadowlands 9.2.7 Light/LightParams/LightData directly from CASC DB2
  // files and adapts them into Noggit's existing in-memory sky tables. No legacy
  // Light*.dbc file is read by this path.
  bool loadShadowlandsSkyDB2Bridge(std::shared_ptr<BlizzardArchive::ClientData> const& client_data);
}
