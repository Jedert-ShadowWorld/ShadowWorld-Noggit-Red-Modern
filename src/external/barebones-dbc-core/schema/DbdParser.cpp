#include "schema/DbdParser.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace dbc
{
namespace
{
std::string Trim(std::string text)
{
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    auto const last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> Split(std::string const& text, char delimiter)
{
    std::vector<std::string> result;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, delimiter))
        result.push_back(Trim(item));
    return result;
}

bool ParseBuildRule(std::string const& token, BuildRange& result)
{
    auto const dash = token.find('-');
    auto const first = Build::Parse(Trim(token.substr(0, dash)));
    auto const last = dash == std::string::npos ? first : Build::Parse(Trim(token.substr(dash + 1)));
    if (!first || !last)
        return false;
    result = { *first, *last };
    return true;
}

SchemaType ParseSchemaType(std::string const& type)
{
    if (type.starts_with("locstring")) return SchemaType::LocalizedString;
    if (type.starts_with("string")) return SchemaType::String;
    if (type.starts_with("float")) return SchemaType::Float;
    return SchemaType::Integer;
}

std::string BaseType(std::string text)
{
    auto const foreign = text.find('<');
    if (foreign != std::string::npos)
        text.resize(foreign);
    return text;
}

std::pair<std::string, std::string> ForeignKey(std::string const& text)
{
    auto const start = text.find('<');
    auto const separator = text.find("::", start == std::string::npos ? 0 : start + 1);
    auto const end = text.find('>', separator == std::string::npos ? 0 : separator + 2);
    if (start == std::string::npos || separator == std::string::npos || end == std::string::npos) return {};
    return { Trim(text.substr(start + 1, separator - start - 1)), Trim(text.substr(separator + 2, end - separator - 2)) };
}

void FinishVersion(DbdFile& result, SchemaVersion& version)
{
    if (!version.fields.empty())
        result.versions.push_back(std::move(version));
    version = {};
}

bool IsClassic(GameVersion version)
{
    return version >= GameVersion::VanillaClassic && version <= GameVersion::MistsClassic;
}

bool BuildFitsProfile(Build const& build, VersionProfile const& profile)
{
    if (profile.id == GameVersion::Forever)
        return true;
    if (build.parts[0] != profile.buildMajor)
        return false;
    if (!IsClassic(profile.id))
    {
        if (profile.buildMajor == 1) return build.parts[1] <= 12;
        if (profile.buildMajor >= 2 && profile.buildMajor <= 5) return build.parts[1] <= 4;
        return true;
    }
    if (profile.buildMajor == 1) return build.parts[1] >= 13;
    return build.parts[1] >= 4;
}
}

std::optional<DbdFile> DbdParser::Parse(std::filesystem::path const& path, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "Could not open definition: " + path.string();
        return std::nullopt;
    }
    std::ostringstream content;
    content << stream.rdbuf();
    return ParseText(content.str(), error);
}

std::optional<DbdFile> DbdParser::ParseText(std::string const& text, std::string& error)
{
    DbdFile result;
    SchemaVersion current;
    bool inColumns = false;
    std::istringstream stream(text);
    std::string raw;
    std::size_t lineNumber = 0;
    while (std::getline(stream, raw))
    {
        ++lineNumber;
        auto const comment = raw.find("//");
        auto line = Trim(raw.substr(0, comment));
        if (line.empty())
            continue;
        if (line == "COLUMNS")
        {
            inColumns = true;
            continue;
        }
        if (line.starts_with("BUILD ") || line.starts_with("LAYOUT ") || line.starts_with("COMMENT "))
            inColumns = false;

        if (inColumns)
        {
            auto const separator = line.find_first_of(" \t");
            if (separator == std::string::npos)
            {
                error = "Invalid column declaration on line " + std::to_string(lineNumber);
                return std::nullopt;
            }
            auto const declaredType = Trim(line.substr(0, separator));
            auto const type = BaseType(declaredType);
            auto const foreign = ForeignKey(declaredType);
            auto name = Trim(line.substr(separator + 1));
            if (!name.empty() && name.back() == '?') name.pop_back();
            result.columns[name] = { name, ParseSchemaType(type), foreign.first, foreign.second };
            continue;
        }

        if (line.starts_with("BUILD "))
        {
            if (!current.fields.empty()) FinishVersion(result, current);
            for (auto const& item : Split(line.substr(6), ','))
            {
                BuildRange range;
                if (!ParseBuildRule(item, range))
                {
                    error = "Invalid BUILD on line " + std::to_string(lineNumber);
                    return std::nullopt;
                }
                current.builds.push_back(range);
            }
            continue;
        }
        if (line.starts_with("LAYOUT "))
        {
            if (!current.fields.empty()) FinishVersion(result, current);
            for (auto const& item : Split(line.substr(7), ','))
            {
                std::uint32_t hash = 0;
                auto const parsed = std::from_chars(item.data(), item.data() + item.size(), hash, 16);
                if (parsed.ec != std::errc {})
                {
                    error = "Invalid LAYOUT on line " + std::to_string(lineNumber);
                    return std::nullopt;
                }
                current.layoutHashes.push_back(hash);
            }
            continue;
        }
        if (line.starts_with("COMMENT "))
            continue;

        SchemaField field;
        if (line.starts_with('$'))
        {
            auto const end = line.find('$', 1);
            if (end == std::string::npos)
            {
                error = "Invalid annotation on line " + std::to_string(lineNumber);
                return std::nullopt;
            }
            for (auto const& annotation : Split(line.substr(1, end - 1), ','))
            {
                if (annotation == "id") field.id = true;
                else if (annotation == "relation") field.relation = true;
                else if (annotation == "noninline") field.nonInline = true;
            }
            line.erase(0, end + 1);
        }
        auto const typeStart = line.find('<');
        auto const arrayStart = line.find('[');
        auto const nameEnd = std::min(typeStart == std::string::npos ? line.size() : typeStart,
            arrayStart == std::string::npos ? line.size() : arrayStart);
        field.name = Trim(line.substr(0, nameEnd));
        if (!field.name.empty() && field.name.back() == '?') field.name.pop_back();
        auto declaration = result.columns.find(field.name);
        if (declaration == result.columns.end())
        {
            error = "Unknown column '" + field.name + "' on line " + std::to_string(lineNumber);
            return std::nullopt;
        }
        field.type = declaration->second.type;
        field.foreignTable = declaration->second.foreignTable;
        field.foreignColumn = declaration->second.foreignColumn;
        if (typeStart != std::string::npos)
        {
            auto const end = line.find('>', typeStart);
            if (end == std::string::npos)
            {
                error = "Invalid bit width on line " + std::to_string(lineNumber);
                return std::nullopt;
            }
            auto width = line.substr(typeStart + 1, end - typeStart - 1);
            if (width.starts_with('u'))
            {
                field.isUnsigned = true;
                width.erase(0, 1);
            }
            auto const parsed = std::from_chars(width.data(), width.data() + width.size(), field.bitWidth);
            if (parsed.ec != std::errc {})
            {
                error = "Invalid bit width on line " + std::to_string(lineNumber);
                return std::nullopt;
            }
        }
        if (arrayStart != std::string::npos)
        {
            auto const end = line.find(']', arrayStart);
            auto const size = line.substr(arrayStart + 1, end - arrayStart - 1);
            auto const parsed = std::from_chars(size.data(), size.data() + size.size(), field.arraySize);
            if (end == std::string::npos || parsed.ec != std::errc {} || field.arraySize == 0)
            {
                error = "Invalid array size on line " + std::to_string(lineNumber);
                return std::nullopt;
            }
        }
        current.fields.push_back(std::move(field));
    }
    FinishVersion(result, current);
    error.clear();
    return result;
}

std::optional<SchemaSelection> SelectSchema(DbdFile const& dbd, VersionProfile const& profile,
    std::optional<Build> requestedBuild, std::optional<std::uint32_t> layoutHash, std::string& error)
{
    SchemaVersion const* best = nullptr;
    Build bestBuild {};
    bool exact = false;
    for (auto const& version : dbd.versions)
    {
        if (layoutHash && (version.layoutHashes.empty() ||
            std::find(version.layoutHashes.begin(), version.layoutHashes.end(), *layoutHash) == version.layoutHashes.end()))
            continue;
        for (auto const& range : version.builds)
        {
            if (requestedBuild)
            {
                if (range.Contains(*requestedBuild))
                {
                    best = &version;
                    bestBuild = *requestedBuild;
                    exact = true;
                    break;
                }
            }
            else if (BuildFitsProfile(range.last, profile) && (!best || range.last > bestBuild))
            {
                best = &version;
                bestBuild = range.last;
            }
        }
        if (exact) break;
    }
    if (!best && requestedBuild && layoutHash)
    {
        for (auto const& version : dbd.versions)
        {
            if (std::find(version.layoutHashes.begin(), version.layoutHashes.end(), *layoutHash) == version.layoutHashes.end())
                continue;
            for (auto const& range : version.builds)
            {
                if ((profile.id == GameVersion::Forever || BuildFitsProfile(range.last, profile)) && (!best || range.last > bestBuild))
                {
                    best = &version;
                    bestBuild = range.last;
                }
            }
        }
    }
    if (!best)
    {
        error = requestedBuild ? "No schema matches build " + requestedBuild->ToString() + "."
                               : "No schema matches the selected game version.";
        return std::nullopt;
    }
    error.clear();
    return SchemaSelection { *best, bestBuild, exact };
}

std::vector<Column> FlattenForWdbc(DbdFile const&, SchemaVersion const& schema, std::optional<Build> build)
{
    static constexpr std::array<std::string_view, 16> locales {
        "enUS", "koKR", "frFR", "deDE", "zhCN", "zhTW", "esES", "esMX",
        "ruRU", "none9", "ptBR", "itIT", "none12", "none13", "none14", "none15"
    };
    std::vector<Column> result;
    for (auto const& field : schema.fields)
    {
        if (field.nonInline)
            continue;
        auto add = [&](std::string name, ValueType type, bool localized = false, std::uint32_t storageBits = 32) {
            auto const padding = field.name.starts_with("Padding_");
            Column column { std::move(name), type, field.id, field.relation, localized, storageBits, padding };
            column.foreignTable = field.foreignTable;
            column.foreignColumn = field.foreignColumn;
            result.push_back(std::move(column));
        };
        if (field.type == SchemaType::LocalizedString)
        {
            // Cataclysm moved localized values into locale-specific files. From
            // 4.0 onward a locstring therefore occupies one string offset.
            if (build && build->parts[0] >= 4)
            {
                add(field.name, ValueType::String, true);
                continue;
            }
            for (auto locale : locales)
                add(field.name + "." + std::string(locale), ValueType::String, true);
            add(field.name + ".mask", ValueType::UnsignedInteger, true);
            continue;
        }
        auto const valueType = field.type == SchemaType::String ? ValueType::String :
            field.type == SchemaType::Float ? ValueType::Float :
            field.isUnsigned ? ValueType::UnsignedInteger : ValueType::SignedInteger;
        for (std::uint32_t index = 0; index < field.arraySize; ++index)
            add(field.arraySize == 1 ? field.name : field.name + "[" + std::to_string(index) + "]", valueType,
                false, field.type == SchemaType::Integer ? field.bitWidth : 32);
    }
    return result;
}

std::vector<Column> FlattenForWdc5(DbdFile const&, SchemaVersion const& schema)
{
    std::vector<Column> result;
    std::uint32_t physicalField = 0;
    for (auto const& field : schema.fields)
    {
        auto const mappedField = field.nonInline ? UINT32_MAX : physicalField++;
        auto const valueType = field.type == SchemaType::String || field.type == SchemaType::LocalizedString
            ? ValueType::String
            : field.type == SchemaType::Float ? ValueType::Float
            : field.isUnsigned ? ValueType::UnsignedInteger : ValueType::SignedInteger;
        for (std::uint32_t index = 0; index < field.arraySize; ++index)
        {
            Column column;
            column.name = field.arraySize == 1 ? field.name : field.name + "[" + std::to_string(index) + "]";
            column.type = valueType;
            column.id = field.id;
            column.relation = field.relation;
            column.localized = field.type == SchemaType::LocalizedString;
            column.storageBits = field.type == SchemaType::Integer ? field.bitWidth : 32;
            column.padding = field.name.starts_with("Padding_");
            column.physicalField = mappedField;
            column.arrayIndex = index;
            column.arraySize = field.arraySize;
            column.nonInline = field.nonInline;
            column.foreignTable = field.foreignTable;
            column.foreignColumn = field.foreignColumn;
            result.push_back(std::move(column));
        }
    }
    return result;
}
}
