#include "core/GameVersion.h"

#include <charconv>
#include <sstream>
#include <vector>

namespace dbc
{
std::string Build::ToString() const
{
    return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
        std::to_string(parts[2]) + "." + std::to_string(parts[3]);
}

std::optional<Build> Build::Parse(std::string_view text)
{
    Build result;
    std::size_t start = 0;
    for (std::size_t index = 0; index < result.parts.size(); ++index)
    {
        auto const end = text.find('.', start);
        auto const token = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        if (token.empty())
            return std::nullopt;
        auto const parsed = std::from_chars(token.data(), token.data() + token.size(), result.parts[index]);
        if (parsed.ec != std::errc {} || parsed.ptr != token.data() + token.size())
            return std::nullopt;
        if (index < 3 && end == std::string_view::npos)
            return std::nullopt;
        if (index == 3 && end != std::string_view::npos)
            return std::nullopt;
        start = end + 1;
    }
    return result;
}

std::array<VersionProfile, 18> const& VersionProfiles()
{
    static constexpr std::array profiles {
        VersionProfile { GameVersion::Vanilla, "Vanilla", 1, "1.12.1.5875" },
        VersionProfile { GameVersion::BurningCrusade, "The Burning Crusade", 2, "2.4.3.8606" },
        VersionProfile { GameVersion::Wrath, "Wrath of the Lich King", 3, "3.3.5.12340" },
        VersionProfile { GameVersion::Cataclysm, "Cataclysm", 4, "4.3.4.15595" },
        VersionProfile { GameVersion::Mists, "Mists of Pandaria", 5, "5.4.8.18414" },
        VersionProfile { GameVersion::Warlords, "Warlords of Draenor", 6, "6.2.4.21742" },
        VersionProfile { GameVersion::Legion, "Legion", 7, "7.3.5.26972" },
        VersionProfile { GameVersion::BattleForAzeroth, "Battle for Azeroth", 8, "8.3.7.35662" },
        VersionProfile { GameVersion::Shadowlands, "Shadowlands", 9, "9.2.7.45745" },
        VersionProfile { GameVersion::Dragonflight, "Dragonflight", 10, "10.2.7.55664" },
        VersionProfile { GameVersion::WarWithin, "The War Within", 11, "" },
        VersionProfile { GameVersion::Midnight, "Midnight", 12, "" },
        VersionProfile { GameVersion::VanillaClassic, "Vanilla Classic", 1, "1.15.3.56626" },
        VersionProfile { GameVersion::BurningCrusadeClassic, "Burning Crusade Classic", 2, "2.5.4.44833" },
        VersionProfile { GameVersion::WrathClassic, "Wrath of the Lich King Classic", 3, "3.4.3.54261" },
        VersionProfile { GameVersion::CataclysmClassic, "Cataclysm Classic", 4, "4.4.2.59185" },
        VersionProfile { GameVersion::MistsClassic, "Mists of Pandaria Classic", 5, "5.5.0.61217" },
        VersionProfile { GameVersion::Forever, "Forever", 0, "" }
    };
    return profiles;
}

VersionProfile const& Profile(GameVersion version)
{
    for (auto const& profile : VersionProfiles())
        if (profile.id == version)
            return profile;
    return VersionProfiles().back();
}
}
