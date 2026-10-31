#pragma once
#include "UidRepair.hpp"
#include <array>
#include <functional>
#include <glm/vec3.hpp>

namespace Noggit::Formats::ADT
{
  struct UidBounds { glm::vec3 min{}, max{}; };
  struct UidTile
  {
    int x = 0, z = 0;
    std::string path;
    UidRepair::Bytes objects, lod;
    bool hasLod = false;
    bool editable = true;
  };
  struct UidProblem { std::string asset, source, reason; std::uint32_t uid; };
  struct UidGeometryPlan
  {
    std::vector<UidTile> tiles;
    // Coordinates whose changed OBJ1 requires terrain LOD aggregate bounds repair.
    std::vector<std::array<int, 2>> changedObjectLodTiles;
    std::vector<UidProblem> problems;
    std::vector<std::uint32_t> sourceUids, targetUids;
    std::uint32_t highest = 0;
    std::size_t removed = 0, references = 0;
  };
  using UidAssetReader = std::function<UidRepair::Bytes(UidRepair::Reference const&)>;
  using UidCanonicalAsset = std::function<void(UidRepair::Record&)>;
  using UidLodWriter = std::function<UidRepair::Bytes(UidRepair::Bytes const&,
      UidRepair::Bytes const&, UidRepair::Bytes const&,
      std::map<std::uint32_t, std::array<float, 6>> const&)>;
  UidBounds readUidAssetBounds(UidRepair::Bytes const&, bool wmo);
  UidGeometryPlan planUidGeometry(std::vector<UidTile> tiles, bool modern,
      bool scaledWmos, UidAssetReader const&, UidCanonicalAsset const&, UidLodWriter const&,
      bool preserveUids = false);
}
