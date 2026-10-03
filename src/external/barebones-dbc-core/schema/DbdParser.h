#pragma once

#include "core/GameVersion.h"
#include "core/Table.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dbc
{
enum class SchemaType { Integer, Float, String, LocalizedString };

struct ColumnDeclaration
{
    std::string name;
    SchemaType type = SchemaType::Integer;
    std::string foreignTable;
    std::string foreignColumn;
};

struct BuildRange
{
    Build first;
    Build last;
    bool Contains(Build const& build) const { return build >= first && build <= last; }
};

struct SchemaField
{
    std::string name;
    SchemaType type = SchemaType::Integer;
    std::uint32_t bitWidth = 32;
    std::uint32_t arraySize = 1;
    bool isUnsigned = false;
    bool id = false;
    bool relation = false;
    bool nonInline = false;
    std::string foreignTable;
    std::string foreignColumn;
};

struct SchemaVersion
{
    std::vector<BuildRange> builds;
    std::vector<std::uint32_t> layoutHashes;
    std::vector<SchemaField> fields;
};

struct DbdFile
{
    std::unordered_map<std::string, ColumnDeclaration> columns;
    std::vector<SchemaVersion> versions;
};

struct SchemaSelection
{
    SchemaVersion version;
    Build selectedBuild;
    bool exactBuild = false;
};

class DbdParser
{
public:
    static std::optional<DbdFile> Parse(std::filesystem::path const& path, std::string& error);
    static std::optional<DbdFile> ParseText(std::string const& text, std::string& error);
};

std::optional<SchemaSelection> SelectSchema(DbdFile const& dbd, VersionProfile const& profile,
    std::optional<Build> requestedBuild, std::optional<std::uint32_t> layoutHash, std::string& error);
std::vector<Column> FlattenForWdbc(DbdFile const& dbd, SchemaVersion const& schema,
    std::optional<Build> build = std::nullopt);
std::vector<Column> FlattenForWdc5(DbdFile const& dbd, SchemaVersion const& schema);
}
