#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dbc
{
enum class ValueType { SignedInteger, UnsignedInteger, Float, String };
enum class ColumnPresentation { Default, Enumeration, Flags };
using CellValue = std::variant<std::int64_t, std::uint64_t, double, std::string>;

struct NamedValue
{
    std::uint64_t value = 0;
    std::string name;
};

struct Column
{
    std::string name;
    ValueType type = ValueType::SignedInteger;
    bool id = false;
    bool relation = false;
    bool localized = false;
    std::uint32_t storageBits = 32;
    bool padding = false;
    // DB2 files store an array as one physical field. These values retain the
    // mapping after arrays are expanded into editable grid columns.
    std::uint32_t physicalField = UINT32_MAX;
    std::uint32_t arrayIndex = 0;
    std::uint32_t arraySize = 1;
    bool nonInline = false;
    std::string foreignTable;
    std::string foreignColumn;
    ColumnPresentation presentation = ColumnPresentation::Default;
    std::vector<NamedValue> namedValues;
    std::optional<double> minimum;
    std::optional<double> maximum;
    bool required = false;
    // Original fixed-record placement retained from WDB5/WDB6 so a matching
    // WCH7/WCH8 ADB can be decoded with the base table's physical layout.
    std::optional<std::uint32_t> fileOffsetBits;
    std::optional<std::uint32_t> fileStorageBits;
};

struct Row
{
    std::uint64_t stableId = 0;
    std::vector<CellValue> cells;
    bool operator==(Row const&) const = default;
};

struct TableFormatState
{
    virtual ~TableFormatState() = default;
};

enum class FileFormat
{
    Unknown, WDBC, WDB2, WDB3, WDB4, WDB5, WDB6, WDC1, WDC2, WDC3, WDC4, WDC5,
    WCH7, WCH8, XFTH, PTCH
};

std::string_view FormatName(FileFormat format);

struct Table
{
    std::filesystem::path path;
    FileFormat format = FileFormat::Unknown;
    std::vector<Column> columns;
    std::vector<Row> rows;
    bool dirty = false;
    std::uint64_t nextStableId = 1;
    bool writable = true;
    std::string readOnlyReason;
    std::shared_ptr<TableFormatState> formatState;
};

std::string CellToString(CellValue const& value);
bool SetCellFromString(CellValue& value, ValueType type, std::string const& text, std::string& error);
}
