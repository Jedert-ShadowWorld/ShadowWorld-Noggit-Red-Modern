// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/Misc.h>
#include <noggit/client_data/ShadowlandsMapDB2Bridge.hpp>
#include <noggit/client_data/ShadowlandsSkyDB2Bridge.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <blizzard-archive-library/include/ClientData.hpp>
#include <string>
#include <Exception.hpp>

AreaDB gAreaDB;
AreaTriggerDB gAreaTriggerDB;
MapDB gMapDB;
LoadingScreensDB gLoadingScreensDB;
LightDB gLightDB;
LightParamsDB gLightParamsDB;
LightSkyboxDB gLightSkyboxDB;
LightIntBandDB gLightIntBandDB;
LightFloatBandDB gLightFloatBandDB;
GroundEffectDoodadDB gGroundEffectDoodadDB;
GroundEffectTextureDB gGroundEffectTextureDB;
TerrainTypeDB gTerrainTypeDB;
LiquidTypeDB gLiquidTypeDB;
SoundProviderPreferencesDB gSoundProviderPreferencesDB;
SoundAmbienceDB gSoundAmbienceDB;
ZoneMusicDB gZoneMusicDB;
ZoneIntroMusicTableDB gZoneIntroMusicTableDB;
SoundEntriesDB gSoundEntriesDB;
WMOAreaTableDB gWMOAreaTableDB;

void OpenDBs(std::shared_ptr<BlizzardArchive::ClientData> clientData)
{
  bool const shadowlands = Noggit::Project::CurrentProject::get()->projectVersion
    == Noggit::Project::ProjectVersion::SL;

  if (shadowlands)
  {
    Log << "Opening Shadowlands client databases from CASC DB2..." << std::endl;

    bool const map_db2_ok = Noggit::ClientData::loadShadowlandsMapDB2Bridge(clientData);
    bool const sky_db2_ok = Noggit::ClientData::loadShadowlandsSkyDB2Bridge(clientData);

    if (!map_db2_ok)
      LogError << "[ModernDB2][Map] Shadowlands map DB2 bootstrap is incomplete." << std::endl;
    if (!sky_db2_ok)
      LogError << "[ModernDB2][Sky] Shadowlands sky DB2 bridge did not load usable data." << std::endl;

    // Critical rule for modern clients: never continue into the WotLK .dbc
    // bootstrap. All map/environment client data must originate from the active
    // Shadowlands CASC/DB2 set.
    return;
  }

  Log << "Opening client DBCs..." << std::endl;

  try
  {
    gAreaDB.open(clientData);
    gAreaTriggerDB.open(clientData);
    gMapDB.open(clientData);
    gLoadingScreensDB.open(clientData);
    gLightDB.open(clientData);
    gLightParamsDB.open(clientData);
    gLightSkyboxDB.open(clientData);
    gLightIntBandDB.open(clientData);
    gLightFloatBandDB.open(clientData);
    gGroundEffectDoodadDB.open(clientData);
    gGroundEffectTextureDB.open(clientData);
    gTerrainTypeDB.open(clientData);
    gLiquidTypeDB.open(clientData);
    gSoundProviderPreferencesDB.open(clientData);
    gSoundAmbienceDB.open(clientData);
    gZoneMusicDB.open(clientData);
    gZoneIntroMusicTableDB.open(clientData);
    gSoundEntriesDB.open(clientData);
    gWMOAreaTableDB.open(clientData);
  }
  catch (BlizzardArchive::Exceptions::FileReadFailedError const& e)
  {
      LogError << e.what() << std::endl;
  }
  catch (...)
  {
      LogError << "OpenDBs() : unhandled exception" << std::endl;
  }

}


// includes the parent zone name as a prefix
std::string AreaDB::getAreaFullName(int pAreaID)
{
  if (!pAreaID || pAreaID == -1)
  {
    return "Unknown location";
  }    

  unsigned int regionID = 0;
  std::string areaName = "";
  try
  {
    AreaDB::Record rec = gAreaDB.getByID(pAreaID);
    areaName = rec.getLocalizedString(AreaDB::Name);
    regionID = rec.getUInt(AreaDB::Region);
  }
  catch (AreaDB::NotFound)
  {
    areaName = "Unknown location";
  }
  if (regionID != 0)
  {
    try
    {
      AreaDB::Record rec = gAreaDB.getByID(regionID);
      areaName = std::string(rec.getLocalizedString(AreaDB::Name)) + std::string(": ") + areaName;
    }
    catch (AreaDB::NotFound)
    {
      areaName = "Unknown location";
    }
  }

  return areaName;
}

std::uint32_t AreaDB::get_area_parent(int area_id)
{
  // todo: differentiate between no parent and error ?
  if (!area_id || area_id == -1)
  {
    return 0;
  }

  try
  {
    AreaDB::Record rec = gAreaDB.getByID(area_id);
    return rec.getUInt(AreaDB::Region);
  }
  catch (AreaDB::NotFound)
  {
    return 0;
  }
}

int AreaDB::resolve_zone_id(int area_id)
{
  std::uint32_t const parent = get_area_parent(area_id);
  return parent ? static_cast<int>(parent) : area_id;
}

std::uint32_t AreaDB::get_new_areabit()
{
    unsigned int areabit = 0;

    for (Iterator i = gAreaDB.begin(); i != gAreaDB.end(); ++i)
    {
        areabit = std::max(i->getUInt(AreaDB::AreaBit), areabit);
    }

    return static_cast<int>(++areabit);
}

std::string MapDB::getMapName(int pMapID)
{
  if (pMapID<0) return "Unknown map";
  std::string mapName = "";
  try
  {
    MapDB::Record rec = gMapDB.getByID(pMapID);
    mapName = std::string(rec.getLocalizedString(MapDB::Name));
  }
  catch (MapDB::NotFound)
  {
    mapName = "Unknown map";
  }

  return mapName;
}

int MapDB::findMapName(const std::string &map_name)
{
  for (Iterator i = gMapDB.begin(); i != gMapDB.end(); ++i)
  {
    if (i->getString(MapDB::InternalName) == map_name)
    {
      return static_cast<int>(i->getUInt(MapDB::MapID));
    }
  }

  return -1;
}

const char * getGroundEffectDoodad(unsigned int effectID, int DoodadNum)
{
  try
  {
    unsigned int doodadId = gGroundEffectTextureDB.getByID(effectID).getUInt(GroundEffectTextureDB::Doodads + DoodadNum);
    return gGroundEffectDoodadDB.getByID(doodadId).getString(GroundEffectDoodadDB::Filename);
  }
  catch (DBCFile::NotFound)
  {
    LogError << "Tried to get a not existing row in GroundEffectTextureDB or GroundEffectDoodadDB ( effectID = " << effectID << ", DoodadNum = " << DoodadNum << " )!" << std::endl;
    return 0;
  }
}

int LiquidTypeDB::getLiquidType(int pID)
{
  int type = 0;
  try
  {
    LiquidTypeDB::Record rec = gLiquidTypeDB.getByID(pID);
    type = rec.getUInt(LiquidTypeDB::Type);
  }
  catch (LiquidTypeDB::NotFound)
  {
    type = 0;
  }
  return type;
}

std::string  LiquidTypeDB::getLiquidName(int pID)
{
  std::string type = "";
  try
  {
    LiquidTypeDB::Record rec = gLiquidTypeDB.getByID(pID);
    type = std::string(rec.getString(LiquidTypeDB::Name));
  }
  catch (MapDB::NotFound)
  {
    type = "Unknown type";
  }

  return type;
}

std::string WMOAreaTableDB::getWMOAreaName(int WMOId, int namesetId)
{
    if (WMOId == -1)
    {
        return "Unknown location";
    }

    for (Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
        if (i->getUInt(WMOAreaTableDB::WmoId) == WMOId && i->getUInt(WMOAreaTableDB::NameSetId) == namesetId && i->getInt(WMOAreaTableDB::WMOGroupID) == -1)
        {
            std::string areaName = i->getLocalizedString(WMOAreaTableDB::Name);

            if (!areaName.empty())
                return areaName;
            else
            {
                int areatableid = i->getUInt(WMOAreaTableDB::AreaTableRefId);
                if (areatableid)
                {
                    std::string arena_name = "";
                    try
                    {
                        auto rec = gAreaDB.getByID(areatableid);
                        arena_name =  rec.getLocalizedString(AreaDB::Name);
                    }
                    catch (WMOAreaTableDB::NotFound)
                    {
                        arena_name = "Unknown location";
                    }
                    return areaName;
                }
                else
                {
                    return "-Local Terrain Area-";
                }

            }
        }
    }
    throw NotFound();
}

std::vector<std::string> WMOAreaTableDB::getWMOAreaNames(int WMOId)
{
    std::vector<std::string> areanamesvect;

    if (WMOId == -1)
    {
        return areanamesvect;
    }

    for (Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
        if (i->getUInt(WMOAreaTableDB::WmoId) == WMOId && i->getInt(WMOAreaTableDB::WMOGroupID) == -1)
        {
            std::string areaName = i->getLocalizedString(WMOAreaTableDB::Name);

            if (!areaName.empty())
                areanamesvect.push_back(areaName);
            else
            {
                int areatableid = i->getUInt(WMOAreaTableDB::AreaTableRefId);
                if (areatableid)
                {
                    try
                    {
                        auto rec = gAreaDB.getByID(areatableid);
                        areanamesvect.push_back(rec.getLocalizedString(AreaDB::Name));
                    }
                    catch (WMOAreaTableDB::NotFound)
                    {
                        areanamesvect.push_back("Unknown location");
                    }

                }
                else
                    areanamesvect.push_back("-Local Terrain Area-");
            }
        }
    }
    return areanamesvect;
}
