#pragma once

#include "core/Table.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dbc
{
struct FileHeaderInfo
{
    FileFormat format = FileFormat::Unknown;
    std::uint32_t recordCount = 0;
    std::uint32_t fieldCount = 0;
    std::uint32_t recordSize = 0;
    std::uint32_t stringBlockSize = 0;
    std::optional<std::uint32_t> layoutHash;
    std::uint32_t tableHash = 0;
    std::uint32_t schemaVersion = 0;
    std::string schemaTag;
    std::uint32_t minId = 0;
    std::uint32_t maxId = 0;
    std::uint16_t flags = 0;
    std::uint16_t idFieldIndex = 0;
    std::uint32_t totalFieldCount = 0;
    std::uint32_t sectionCount = 0;
};

struct Wdc5Verification
{
    FileHeaderInfo header;
    std::uint32_t decodedSectionRecords = 0;
    std::uint32_t encryptedSections = 0;
    std::uint32_t sparseSections = 0;
    std::uint32_t copyRows = 0;
};

struct PtchVerification
{
    std::uint32_t decompressedPatchSize = 0;
    std::uint32_t sizeBefore = 0;
    std::uint32_t sizeAfter = 0;
    std::uint32_t transformBlockSize = 0;
    std::string patchType;
    bool compressed = false;
};

FileHeaderInfo InspectHeader(std::filesystem::path const& path, std::string& error);
std::optional<Wdc5Verification> VerifyWdc5(std::filesystem::path const& path, std::string& error);
std::optional<PtchVerification> VerifyPtch(std::filesystem::path const& path, std::string& error);

class BinaryTableCodec
{
public:
    static std::optional<Table> LoadWdbc(std::filesystem::path const& path,
        std::vector<Column> columns, std::string& error);
    static bool SaveWdbc(Table const& table, std::filesystem::path const& path, std::string& error);
    static std::optional<Table> LoadWdb(std::filesystem::path const& path,
        std::vector<Column> columns, std::string& error);
    static bool SaveWdb(Table const& table, std::filesystem::path const& path, std::string& error);
    static std::optional<Table> LoadWdc(std::filesystem::path const& path,
        std::vector<Column> columns, std::string& error);
    static bool SaveWdc(Table const& table, std::filesystem::path const& path, std::string& error);
    static std::optional<Table> LoadWdc5(std::filesystem::path const& path,
        std::vector<Column> columns, std::string& error);
    static bool SaveWdc5(Table const& table, std::filesystem::path const& path, std::string& error);
};
}
