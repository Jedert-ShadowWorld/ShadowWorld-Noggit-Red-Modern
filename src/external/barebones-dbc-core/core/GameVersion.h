#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dbc
{
struct Build
{
    std::array<std::uint32_t, 4> parts {};
    auto operator<=>(Build const&) const = default;
    std::string ToString() const;
    static std::optional<Build> Parse(std::string_view text);
};

enum class GameVersion
{
    Vanilla, BurningCrusade, Wrath, Cataclysm, Mists, Warlords, Legion,
    BattleForAzeroth, Shadowlands, Dragonflight, WarWithin, Midnight,
    VanillaClassic, BurningCrusadeClassic, WrathClassic, CataclysmClassic,
    MistsClassic, Forever
};

struct VersionProfile
{
    GameVersion id;
    std::string_view name;
    std::uint32_t buildMajor;
    std::string_view suggestedBuild;
};

std::array<VersionProfile, 18> const& VersionProfiles();
VersionProfile const& Profile(GameVersion version);
}
