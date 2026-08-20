// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once

#include <memory>
#include <stdexcept>
#include <type_traits>

namespace BlizzardArchive
{
  class ClientData;
}

namespace Noggit::ClientData
{
  // Load map/editor related Shadowlands DB2s from the active CASC and adapt the
  // tables still consumed through Noggit's legacy DBC-shaped in-memory APIs.
  // No .dbc files are opened on this path.
  bool loadShadowlandsMapDB2Bridge(std::shared_ptr<BlizzardArchive::ClientData> const& client_data);
}
