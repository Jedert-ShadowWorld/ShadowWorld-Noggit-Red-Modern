#include "formats/BinaryTable.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>

namespace dbc
{
namespace
{
constexpr std::uint16_t FlagSparse = 0x1;
constexpr std::uint16_t FlagSecondaryKey = 0x2;
constexpr std::uint16_t FlagIndex = 0x4;

struct FieldMeta
{
    std::int16_t bits = 0;
    std::int16_t offset = 0;
};

struct LegacyDb2State final : TableFormatState
{
    FileFormat format = FileFormat::Unknown;
    std::uint32_t tableHash = 0;
    std::uint32_t build = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t layoutHash = 0;
    std::uint32_t locale = 0;
    std::uint16_t flags = 0;
    std::uint16_t idFieldIndex = 0;
    std::uint32_t inlineFieldCount = 0;
    std::uint32_t totalFieldCount = 0;
    std::vector<FieldMeta> fields;
};

struct SparseRecord
{
    std::int32_t id = -1;
    std::uint32_t offset = 0;
    std::uint16_t size = 0;
};

bool Range(std::vector<unsigned char> const& bytes, std::size_t offset, std::size_t size)
{
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

std::uint64_t ReadUnsigned(std::vector<unsigned char> const& bytes, std::size_t offset, std::size_t count)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
    return value;
}

std::uint16_t U16(std::vector<unsigned char> const& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(ReadUnsigned(bytes, offset, 2));
}

std::uint32_t U32(std::vector<unsigned char> const& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(ReadUnsigned(bytes, offset, 4));
}

std::uint64_t ReadBits(std::vector<unsigned char> const& bytes, std::size_t byteOffset,
    std::size_t bitOffset, std::uint32_t count)
{
    std::uint64_t value = 0;
    for (std::uint32_t bit = 0; bit < count; ++bit)
    {
        auto const position = bitOffset + bit;
        if ((bytes[byteOffset + position / 8] & (1u << (position % 8))) != 0)
            value |= std::uint64_t { 1 } << bit;
    }
    return value;
}

std::int64_t SignExtend(std::uint64_t value, std::uint32_t bits)
{
    if (bits == 0) return 0;
    if (bits >= 64) return static_cast<std::int64_t>(value);
    value &= (std::uint64_t { 1 } << bits) - 1;
    auto const sign = std::uint64_t { 1 } << (bits - 1);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

std::uint64_t CellBits(CellValue const& value)
{
    if (auto item = std::get_if<std::int64_t>(&value)) return static_cast<std::uint64_t>(*item);
    if (auto item = std::get_if<std::uint64_t>(&value)) return *item;
    if (auto item = std::get_if<double>(&value)) return std::bit_cast<std::uint32_t>(static_cast<float>(*item));
    return 0;
}

CellValue NumericCell(std::uint64_t raw, Column const& column, std::uint32_t bits)
{
    if (column.type == ValueType::Float)
        return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
    if (column.type == ValueType::UnsignedInteger) return raw;
    return SignExtend(raw, std::min(bits, column.storageBits));
}

std::string ReadCString(std::vector<unsigned char> const& bytes, std::size_t offset, std::size_t end)
{
    if (offset >= end || end > bytes.size()) return {};
    auto cursor = offset;
    while (cursor < end && bytes[cursor] != 0) ++cursor;
    return std::string(reinterpret_cast<char const*>(bytes.data() + offset), cursor - offset);
}

void WriteU16(std::ostream& stream, std::uint16_t value)
{
    unsigned char bytes[2] { static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8) };
    stream.write(reinterpret_cast<char const*>(bytes), 2);
}

void WriteU32(std::ostream& stream, std::uint32_t value)
{
    unsigned char bytes[4] { static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24) };
    stream.write(reinterpret_cast<char const*>(bytes), 4);
}

void WriteBits(std::vector<unsigned char>& bytes, std::size_t bitOffset, std::uint64_t value, std::uint32_t count)
{
    auto const required = (bitOffset + count + 7) / 8;
    if (bytes.size() < required) bytes.resize(required);
    for (std::uint32_t bit = 0; bit < count; ++bit)
    {
        auto const mask = static_cast<unsigned char>(1u << ((bitOffset + bit) % 8));
        auto& target = bytes[(bitOffset + bit) / 8];
        if ((value & (std::uint64_t { 1 } << bit)) != 0) target |= mask;
        else target &= static_cast<unsigned char>(~mask);
    }
}

std::optional<std::int32_t> RowId(Row const& row, std::vector<Column> const& columns)
{
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (!columns[i].id) continue;
        if (auto item = std::get_if<std::int64_t>(&row.cells[i]))
        {
            if (*item < std::numeric_limits<std::int32_t>::min() || *item > std::numeric_limits<std::int32_t>::max()) return {};
            return static_cast<std::int32_t>(*item);
        }
        if (auto item = std::get_if<std::uint64_t>(&row.cells[i]))
        {
            if (*item > std::numeric_limits<std::uint32_t>::max()) return {};
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(*item));
        }
    }
    return {};
}

std::uint32_t FieldWidth(Column const& column)
{
    return column.type == ValueType::String || column.type == ValueType::Float ? 32u : column.storageBits;
}

std::vector<std::size_t> PhysicalStarts(std::vector<Column> const& columns)
{
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < columns.size(); ++i)
        if (!columns[i].nonInline && columns[i].arrayIndex == 0) result.push_back(i);
    std::sort(result.begin(), result.end(), [&](auto left, auto right) {
        return columns[left].physicalField < columns[right].physicalField;
    });
    return result;
}

bool ReplaceWithBackup(std::filesystem::path const& temporary, std::filesystem::path const& destination,
    std::string& error)
{
    std::error_code fsError;
    if (std::filesystem::exists(destination))
    {
        auto backup = destination;
        backup += ".bak";
        std::filesystem::copy_file(destination, backup, std::filesystem::copy_options::overwrite_existing, fsError);
        if (fsError) { error = "Could not create backup: " + fsError.message(); return false; }
    }
    std::filesystem::rename(temporary, destination, fsError);
    if (fsError)
    {
        std::filesystem::remove(destination, fsError);
        fsError.clear();
        std::filesystem::rename(temporary, destination, fsError);
    }
    if (fsError) { error = "Could not replace destination: " + fsError.message(); return false; }
    return true;
}

char const* Magic(FileFormat format)
{
    switch (format)
    {
    case FileFormat::WDB2: return "WDB2";
    case FileFormat::WDB3: return "WDB3";
    case FileFormat::WDB4: return "WDB4";
    case FileFormat::WDB5: return "WDB5";
    case FileFormat::WDB6: return "WDB6";
    default: return "";
    }
}
}

std::optional<Table> BinaryTableCodec::LoadWdb(std::filesystem::path const& path,
    std::vector<Column> columns, std::string& error)
{
    auto fail = [&](std::string message) -> std::optional<Table> { error = std::move(message); return std::nullopt; };
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return fail("Could not open " + path.string());
    auto const length = stream.tellg();
    if (length < 20) return fail("The DB2 file is truncated.");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!stream) return fail("Could not read " + path.string());

    std::string inspectError;
    auto const header = InspectHeader(path, inspectError);
    if (header.format < FileFormat::WDB2 || header.format > FileFormat::WDB6)
        return fail("Expected a WDB2-WDB6 table.");

    auto state = std::make_shared<LegacyDb2State>();
    state->format = header.format;
    auto const recordCount = U32(bytes, 4);
    auto const fieldCount = U32(bytes, 8);
    auto const recordSize = U32(bytes, 12);
    auto const stringSize = U32(bytes, 16);
    state->inlineFieldCount = fieldCount;
    state->totalFieldCount = fieldCount;
    std::uint32_t minId = 0, maxId = 0, copySize = 0, commonSize = 0;
    std::size_t cursor = 20;

    if (state->format == FileFormat::WDB2)
    {
        if (!Range(bytes, 0, 32)) return fail("The WDB2 header is truncated.");
        state->tableHash = U32(bytes, 20); state->build = U32(bytes, 24); state->timestamp = U32(bytes, 28);
        cursor = 32;
        if (state->build > 12880)
        {
            if (!Range(bytes, cursor, 16)) return fail("The WDB2 extended header is truncated.");
            minId = U32(bytes, cursor); maxId = U32(bytes, cursor + 4);
            state->layoutHash = U32(bytes, cursor + 8); copySize = U32(bytes, cursor + 12); cursor += 16;
            if (maxId >= minId && maxId != 0)
            {
                auto const count = static_cast<std::uint64_t>(maxId) - minId + 1;
                if (count > 100000000 || !Range(bytes, cursor, static_cast<std::size_t>(count * 6)))
                    return fail("The WDB2 ID lookup is truncated.");
                cursor += static_cast<std::size_t>(count * 6);
            }
        }
    }
    else if (state->format == FileFormat::WDB3 || state->format == FileFormat::WDB4)
    {
        auto const headerSize = state->format == FileFormat::WDB3 ? 48u : 52u;
        if (!Range(bytes, 0, headerSize)) return fail("The legacy DB2 header is truncated.");
        state->tableHash = U32(bytes, 20); state->build = U32(bytes, 24); state->timestamp = U32(bytes, 28);
        minId = U32(bytes, 32); maxId = U32(bytes, 36); state->locale = U32(bytes, 40); copySize = U32(bytes, 44);
        if (state->format == FileFormat::WDB4) state->flags = static_cast<std::uint16_t>(U32(bytes, 48));
        else if (state->tableHash == 3348406326u || state->tableHash == 2442913102u) state->flags = FlagSparse;
        else if (state->tableHash == 2982519032u) state->flags = FlagSparse | FlagSecondaryKey;
        cursor = headerSize;
    }
    else
    {
        auto const headerSize = state->format == FileFormat::WDB5 ? 48u : 56u;
        if (!Range(bytes, 0, headerSize)) return fail("The legacy DB2 header is truncated.");
        state->tableHash = U32(bytes, 20); state->layoutHash = U32(bytes, 24);
        minId = U32(bytes, 28); maxId = U32(bytes, 32); state->locale = U32(bytes, 36); copySize = U32(bytes, 40);
        state->flags = U16(bytes, 44); state->idFieldIndex = U16(bytes, 46);
        if (state->format == FileFormat::WDB6)
        {
            state->totalFieldCount = U32(bytes, 48); commonSize = U32(bytes, 52);
        }
        cursor = headerSize;
        if (!Range(bytes, cursor, static_cast<std::size_t>(fieldCount) * 4)) return fail("The DB2 field metadata is truncated.");
        state->fields.resize(fieldCount);
        for (std::uint32_t i = 0; i < fieldCount; ++i)
        {
            state->fields[i].bits = static_cast<std::int16_t>(U16(bytes, cursor + i * 4));
            state->fields[i].offset = static_cast<std::int16_t>(U16(bytes, cursor + i * 4 + 2));
        }
        cursor += static_cast<std::size_t>(fieldCount) * 4;
    }

    auto physical = PhysicalStarts(columns);
    auto const expectedFields = state->format == FileFormat::WDB6 ? state->totalFieldCount : fieldCount;
    if (physical.size() != expectedFields)
        return fail("The selected definition maps " + std::to_string(physical.size()) + " physical fields, but the DB2 header declares " +
            std::to_string(expectedFields) + ".");
    for (std::size_t i = 0; i < physical.size(); ++i)
        if (columns[physical[i]].physicalField != i) return fail("The selected definition has a non-contiguous physical field map.");
    if (state->format >= FileFormat::WDB5)
    {
        for (std::size_t p = 0; p < physical.size() && p < state->fields.size(); ++p)
        {
            auto const firstIndex = physical[p];
            auto const bits = 32u - static_cast<std::int32_t>(state->fields[p].bits);
            auto const offset = static_cast<std::uint16_t>(state->fields[p].offset) * 8u;
            for (std::uint32_t arrayIndex = 0; arrayIndex < columns[firstIndex].arraySize; ++arrayIndex)
            {
                auto const columnIndex = firstIndex + arrayIndex;
                if (columnIndex >= columns.size()) break;
                columns[columnIndex].fileOffsetBits = offset + arrayIndex * bits;
                columns[columnIndex].fileStorageBits = bits;
            }
        }
    }

    bool const sparse = (state->flags & FlagSparse) != 0;
    std::vector<SparseRecord> sparseRecords;
    std::vector<std::pair<std::int32_t, std::int32_t>> sparseCopies;
    std::size_t recordsStart = cursor;
    std::size_t stringsStart = 0;
    std::size_t afterStrings = 0;
    auto const rangeCount = maxId >= minId ? static_cast<std::uint64_t>(maxId) - minId + 1 : 0;
    if (rangeCount > 100000000) return fail("The DB2 ID range is unreasonably large.");

    auto readSparseMap = [&](std::size_t offset) -> bool {
        if (!Range(bytes, offset, static_cast<std::size_t>(rangeCount * 6))) return false;
        std::unordered_map<std::uint64_t, std::int32_t> seen;
        for (std::uint64_t i = 0; i < rangeCount; ++i)
        {
            auto const recordOffset = U32(bytes, offset + static_cast<std::size_t>(i * 6));
            auto const size = U16(bytes, offset + static_cast<std::size_t>(i * 6 + 4));
            if (recordOffset == 0 || size == 0) continue;
            auto const id = static_cast<std::int32_t>(minId + i);
            auto const key = (static_cast<std::uint64_t>(recordOffset) << 16) | size;
            if (auto found = seen.find(key); found != seen.end()) sparseCopies.emplace_back(id, found->second);
            else { seen[key] = id; sparseRecords.push_back({ id, recordOffset, size }); }
        }
        return true;
    };

    std::vector<std::int32_t> relationById;
    if (sparse)
    {
        std::size_t mapOffset = 0;
        if (state->format == FileFormat::WDB3)
        {
            mapOffset = cursor;
            if (!readSparseMap(mapOffset)) return fail("The WDB3 sparse offset map is truncated.");
            cursor += static_cast<std::size_t>(rangeCount * 6);
            if ((state->flags & FlagSecondaryKey) != 0)
            {
                if (!Range(bytes, cursor, static_cast<std::size_t>(rangeCount * 4))) return fail("The WDB3 relationship map is truncated.");
                relationById.resize(static_cast<std::size_t>(rangeCount));
                for (std::size_t i = 0; i < relationById.size(); ++i) relationById[i] = static_cast<std::int32_t>(U32(bytes, cursor + i * 4));
                cursor += static_cast<std::size_t>(rangeCount * 4);
            }
            recordsStart = cursor;
            afterStrings = recordsStart;
            for (auto const& item : sparseRecords) afterStrings = std::max(afterStrings, static_cast<std::size_t>(item.offset) + item.size);
        }
        else
        {
            mapOffset = stringSize;
            if (mapOffset < cursor || !readSparseMap(mapOffset)) return fail("The DB2 sparse offset map is truncated.");
            afterStrings = mapOffset + static_cast<std::size_t>(rangeCount * 6);
        }
    }
    else
    {
        if (state->format == FileFormat::WDB3 && (state->flags & FlagSecondaryKey) != 0)
        {
            if (!Range(bytes, cursor, static_cast<std::size_t>(rangeCount * 4))) return fail("The WDB3 relationship map is truncated.");
            relationById.resize(static_cast<std::size_t>(rangeCount));
            for (std::size_t i = 0; i < relationById.size(); ++i) relationById[i] = static_cast<std::int32_t>(U32(bytes, cursor + i * 4));
            cursor += static_cast<std::size_t>(rangeCount * 4);
            recordsStart = cursor;
        }
        auto const recordBytes = static_cast<std::uint64_t>(recordCount) * recordSize;
        if (recordBytes > std::numeric_limits<std::size_t>::max() || !Range(bytes, recordsStart, static_cast<std::size_t>(recordBytes)))
            return fail("The DB2 record block is truncated.");
        stringsStart = recordsStart + static_cast<std::size_t>(recordBytes);
        if (!Range(bytes, stringsStart, stringSize)) return fail("The DB2 string block is truncated.");
        afterStrings = stringsStart + stringSize;
    }

    auto aux = afterStrings;
    if (state->format != FileFormat::WDB3 && (state->flags & FlagSecondaryKey) != 0)
    {
        if (!Range(bytes, aux, static_cast<std::size_t>(rangeCount * 4))) return fail("The DB2 relationship map is truncated.");
        relationById.resize(static_cast<std::size_t>(rangeCount));
        for (std::size_t i = 0; i < relationById.size(); ++i) relationById[i] = static_cast<std::int32_t>(U32(bytes, aux + i * 4));
        aux += static_cast<std::size_t>(rangeCount * 4);
    }

    bool hasIndex = (state->flags & FlagIndex) != 0;
    if (state->format == FileFormat::WDB3 && !sparse)
    {
        auto const remaining = bytes.size() >= aux + copySize ? bytes.size() - aux - copySize : 0;
        hasIndex = remaining >= static_cast<std::size_t>(recordCount) * 4 + commonSize;
        if (hasIndex) state->flags |= FlagIndex;
    }
    std::vector<std::int32_t> ids;
    if (hasIndex)
    {
        if (!Range(bytes, aux, static_cast<std::size_t>(recordCount) * 4)) return fail("The DB2 index table is truncated.");
        ids.resize(recordCount);
        for (std::uint32_t i = 0; i < recordCount; ++i) ids[i] = static_cast<std::int32_t>(U32(bytes, aux + i * 4));
        aux += static_cast<std::size_t>(recordCount) * 4;
    }

    if (!Range(bytes, aux, copySize) || copySize % 8 != 0) return fail("The DB2 copy table is truncated.");
    std::vector<std::pair<std::int32_t, std::int32_t>> copies;
    for (std::uint32_t i = 0; i < copySize / 8; ++i)
        copies.emplace_back(static_cast<std::int32_t>(U32(bytes, aux + i * 8)), static_cast<std::int32_t>(U32(bytes, aux + i * 8 + 4)));
    aux += copySize;
    copies.insert(copies.end(), sparseCopies.begin(), sparseCopies.end());

    std::vector<std::unordered_map<std::int32_t, std::uint32_t>> common(expectedFields);
    if (state->format == FileFormat::WDB6 && commonSize != 0)
    {
        if (!Range(bytes, aux, commonSize) || commonSize < 4) return fail("The WDB6 common-data block is truncated.");
        auto commonCursor = aux;
        auto const commonEnd = aux + commonSize;
        auto const commonFields = U32(bytes, commonCursor); commonCursor += 4;
        bool const aligned = commonSize >= 4 + commonFields * 5 && (commonSize - 4 - commonFields * 5) % 8 == 0;
        for (std::uint32_t field = 0; field < commonFields; ++field)
        {
            if (!Range(bytes, commonCursor, 5)) return fail("The WDB6 common-data header is truncated.");
            auto const count = U32(bytes, commonCursor); auto const type = bytes[commonCursor + 4]; commonCursor += 5;
            auto const valueSize = aligned ? 4u : type == 1 ? 2u : type == 2 ? 1u : 4u;
            if (field >= common.size() || !Range(bytes, commonCursor, static_cast<std::size_t>(count) * (4 + valueSize)))
                return fail("The WDB6 common-data values are truncated.");
            for (std::uint32_t i = 0; i < count; ++i)
            {
                auto const id = static_cast<std::int32_t>(U32(bytes, commonCursor)); commonCursor += 4;
                common[field][id] = static_cast<std::uint32_t>(ReadUnsigned(bytes, commonCursor, valueSize)); commonCursor += valueSize;
            }
        }
        if (commonCursor > commonEnd) return fail("The WDB6 common-data block exceeds its declared size.");
    }

    Table table;
    table.path = path; table.format = state->format; table.columns = std::move(columns); table.formatState = state;
    std::unordered_map<std::int32_t, std::size_t> rowsById;
    auto const rowTotal = sparse ? sparseRecords.size() : static_cast<std::size_t>(recordCount);
    if (sparse && rowTotal != recordCount) return fail("The sparse offset map does not contain the declared number of records.");
    for (std::size_t rowIndex = 0; rowIndex < rowTotal; ++rowIndex)
    {
        auto const recordOffset = sparse ? static_cast<std::size_t>(sparseRecords[rowIndex].offset)
                                         : recordsStart + rowIndex * recordSize;
        auto const recordLimit = sparse ? recordOffset + sparseRecords[rowIndex].size : recordOffset + recordSize;
        if (!Range(bytes, recordOffset, recordLimit - recordOffset)) return fail("A DB2 record extends past the end of the file.");
        std::int32_t id = sparse ? sparseRecords[rowIndex].id : (rowIndex < ids.size() ? ids[rowIndex] : -1);

        if (id == -1)
        {
            auto found = std::find_if(physical.begin(), physical.end(), [&](auto index) { return table.columns[index].id; });
            if (found == physical.end()) return fail("The DB2 file has neither an index table nor an inline ID field.");
            auto const p = static_cast<std::size_t>(found - physical.begin());
            std::uint32_t bits = FieldWidth(table.columns[*found]);
            std::size_t position = 0;
            if (state->format >= FileFormat::WDB5 && p < state->fields.size())
            {
                bits = 32u - static_cast<std::int32_t>(state->fields[p].bits);
                position = static_cast<std::uint16_t>(state->fields[p].offset) * 8u;
            }
            else for (std::size_t before = 0; before < p; ++before)
                position += static_cast<std::size_t>(FieldWidth(table.columns[physical[before]])) * table.columns[physical[before]].arraySize;
            if (recordOffset + (position + bits + 7) / 8 > recordLimit) return fail("The inline DB2 ID is outside its record.");
            id = static_cast<std::int32_t>(ReadBits(bytes, recordOffset, position, bits));
        }

        Row row; row.stableId = table.nextStableId++; row.cells.resize(table.columns.size());
        std::size_t bitCursor = 0;
        for (std::size_t p = 0; p < physical.size(); ++p)
        {
            auto const firstIndex = physical[p];
            auto const& first = table.columns[firstIndex];
            std::uint32_t bits = FieldWidth(first);
            if (state->format >= FileFormat::WDB5 && p < state->fields.size())
            {
                bits = 32u - static_cast<std::int32_t>(state->fields[p].bits);
                if (!sparse) bitCursor = static_cast<std::uint16_t>(state->fields[p].offset) * 8u;
            }
            for (std::uint32_t arrayIndex = 0; arrayIndex < first.arraySize; ++arrayIndex)
            {
                auto const columnIndex = firstIndex + arrayIndex;
                if (columnIndex >= table.columns.size()) return fail("The DB2 array definition is incomplete.");
                if (p >= state->inlineFieldCount && state->format == FileFormat::WDB6)
                {
                    auto found = common[p].find(id);
                    auto const raw = found == common[p].end() ? 0u : found->second;
                    row.cells[columnIndex] = NumericCell(raw, table.columns[columnIndex], table.columns[columnIndex].storageBits);
                    continue;
                }
                if (first.type == ValueType::String && sparse)
                {
                    if (bitCursor % 8 != 0) return fail("A sparse DB2 string is not byte-aligned.");
                    auto const offset = recordOffset + bitCursor / 8;
                    auto value = ReadCString(bytes, offset, recordLimit);
                    row.cells[columnIndex] = value;
                    bitCursor += (value.size() + 1) * 8;
                }
                else
                {
                    if (recordOffset + (bitCursor + bits + 7) / 8 > recordLimit) return fail("A DB2 field extends past its record.");
                    auto const raw = ReadBits(bytes, recordOffset, bitCursor, bits);
                    if (first.type == ValueType::String)
                    {
                        auto const stringOffset = static_cast<std::size_t>(raw);
                        row.cells[columnIndex] = stringOffset < stringSize
                            ? CellValue { ReadCString(bytes, stringsStart + stringOffset, stringsStart + stringSize) }
                            : CellValue { std::string {} };
                    }
                    else
                    {
                        auto commonFound = p < common.size() ? common[p].find(id) : common[0].end();
                        row.cells[columnIndex] = NumericCell(commonFound != common[p].end() ? commonFound->second : raw,
                            table.columns[columnIndex], bits);
                    }
                    bitCursor += bits;
                }
            }
        }
        for (std::size_t i = 0; i < table.columns.size(); ++i)
        {
            auto const& column = table.columns[i];
            if (!column.nonInline) continue;
            std::int32_t value = 0;
            if (column.id) value = id;
            else if (column.relation && id >= static_cast<std::int32_t>(minId) && static_cast<std::uint64_t>(id - minId) < relationById.size())
                value = relationById[static_cast<std::size_t>(id - minId)];
            row.cells[i] = column.type == ValueType::UnsignedInteger
                ? CellValue { static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)) }
                : CellValue { static_cast<std::int64_t>(value) };
        }
        rowsById[id] = table.rows.size(); table.rows.push_back(std::move(row));
    }

    for (auto const& [destination, source] : copies)
    {
        auto found = rowsById.find(source);
        if (found == rowsById.end()) continue;
        Row row = table.rows[found->second]; row.stableId = table.nextStableId++;
        for (std::size_t i = 0; i < table.columns.size(); ++i)
            if (table.columns[i].id) row.cells[i] = table.columns[i].type == ValueType::UnsignedInteger
                ? CellValue { static_cast<std::uint64_t>(static_cast<std::uint32_t>(destination)) }
                : CellValue { static_cast<std::int64_t>(destination) };
        rowsById[destination] = table.rows.size(); table.rows.push_back(std::move(row));
    }
    error.clear();
    return table;
}

bool BinaryTableCodec::SaveWdb(Table const& table, std::filesystem::path const& path, std::string& error)
{
    auto state = std::dynamic_pointer_cast<LegacyDb2State>(table.formatState);
    if (!state || state->format != table.format) { error = "The legacy DB2 editing state is missing; reopen the source file before saving."; return false; }
    if (table.format < FileFormat::WDB2 || table.format > FileFormat::WDB6) { error = "Expected a WDB2-WDB6 table."; return false; }
    auto physical = PhysicalStarts(table.columns);
    if (physical.empty() && !table.rows.empty()) { error = "The DB2 definition contains no inline fields."; return false; }

    std::vector<std::int32_t> ids; ids.reserve(table.rows.size());
    std::unordered_map<std::int32_t, bool> uniqueIds;
    for (auto const& row : table.rows)
    {
        auto id = RowId(row, table.columns);
        if (!id) { error = "Every DB2 row needs an ID that fits in 32 bits."; return false; }
        if (!uniqueIds.emplace(*id, true).second) { error = "Row ID " + std::to_string(*id) + " is duplicated."; return false; }
        ids.push_back(*id);
    }

    auto nonInlineId = std::find_if(table.columns.begin(), table.columns.end(), [](auto const& column) { return column.id && column.nonInline; });
    auto relation = std::find_if(table.columns.begin(), table.columns.end(), [](auto const& column) { return column.relation && column.nonInline; });
    auto flags = static_cast<std::uint16_t>(state->flags & ~(FlagSparse | FlagSecondaryKey | FlagIndex));
    if (nonInlineId != table.columns.end()) flags |= FlagIndex;
    if (relation != table.columns.end()) flags |= FlagSecondaryKey;

    std::vector<char> strings { '\0' };
    std::unordered_map<std::string, std::uint32_t> stringOffsets { { "", 0 } };
    for (auto const& row : table.rows)
        for (std::size_t i = 0; i < table.columns.size(); ++i)
            if (!table.columns[i].nonInline && table.columns[i].type == ValueType::String)
            {
                auto const& value = std::get<std::string>(row.cells[i]);
                if (stringOffsets.contains(value)) continue;
                if (strings.size() + value.size() + 1 > std::numeric_limits<std::uint32_t>::max()) { error = "The DB2 string table is too large."; return false; }
                stringOffsets[value] = static_cast<std::uint32_t>(strings.size());
                strings.insert(strings.end(), value.begin(), value.end()); strings.push_back('\0');
            }

    std::vector<FieldMeta> metadata;
    metadata.reserve(physical.size());
    std::size_t recordBits = 0;
    for (auto firstIndex : physical)
    {
        auto const& first = table.columns[firstIndex];
        auto const bits = FieldWidth(first);
        if (bits == 0 || bits > 64) { error = "Column '" + first.name + "' has an unsupported storage width."; return false; }
        if (table.format >= FileFormat::WDB5)
        {
            if (recordBits % 8 != 0 || recordBits / 8 > std::numeric_limits<std::int16_t>::max())
            { error = "The WDB5/WDB6 record layout is not byte-addressable."; return false; }
            metadata.push_back({ static_cast<std::int16_t>(32 - static_cast<std::int32_t>(bits)), static_cast<std::int16_t>(recordBits / 8) });
        }
        recordBits += static_cast<std::size_t>(bits) * first.arraySize;
    }
    if (recordBits % 8 != 0 || recordBits / 8 > std::numeric_limits<std::uint32_t>::max())
    { error = "The DB2 record size is not byte-aligned."; return false; }
    auto const recordSize = static_cast<std::uint32_t>(recordBits / 8);

    std::vector<std::vector<unsigned char>> records;
    records.reserve(table.rows.size());
    for (auto const& row : table.rows)
    {
        std::vector<unsigned char> record(recordSize);
        std::size_t bitCursor = 0;
        for (auto firstIndex : physical)
        {
            auto const& first = table.columns[firstIndex];
            auto const bits = FieldWidth(first);
            for (std::uint32_t arrayIndex = 0; arrayIndex < first.arraySize; ++arrayIndex)
            {
                auto const columnIndex = firstIndex + arrayIndex;
                auto const& cell = row.cells[columnIndex];
                std::uint64_t raw = 0;
                if (first.type == ValueType::String) raw = stringOffsets[std::get<std::string>(cell)];
                else raw = CellBits(cell);
                if (bits < 64 && (raw >> bits) != 0)
                {
                    if (auto signedValue = std::get_if<std::int64_t>(&cell))
                    {
                        auto const minimum = -(std::int64_t { 1 } << (bits - 1));
                        auto const maximum = (std::int64_t { 1 } << (bits - 1)) - 1;
                        if (*signedValue < minimum || *signedValue > maximum)
                        { error = "Value in column '" + table.columns[columnIndex].name + "' does not fit its DB2 field."; return false; }
                    }
                    else { error = "Value in column '" + table.columns[columnIndex].name + "' does not fit its DB2 field."; return false; }
                }
                WriteBits(record, bitCursor, raw, bits); bitCursor += bits;
            }
        }
        records.push_back(std::move(record));
    }

    std::int32_t minimum = 0, maximum = 0;
    if (!ids.empty()) { auto bounds = std::minmax_element(ids.begin(), ids.end()); minimum = *bounds.first; maximum = *bounds.second; }
    auto const rangeCount = ids.empty() ? 0ull : static_cast<std::uint64_t>(static_cast<std::uint32_t>(maximum) - static_cast<std::uint32_t>(minimum)) + 1;
    if ((flags & FlagSecondaryKey) != 0 && rangeCount > 100000000)
    { error = "The DB2 relationship map would be unreasonably large because the row IDs are too far apart."; return false; }

    auto temporary = path; temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { error = "Could not create temporary output file."; return false; }
    output.write(Magic(table.format), 4);
    WriteU32(output, static_cast<std::uint32_t>(records.size()));
    WriteU32(output, static_cast<std::uint32_t>(physical.size()));
    WriteU32(output, recordSize);
    WriteU32(output, static_cast<std::uint32_t>(strings.size()));

    if (table.format == FileFormat::WDB2)
    {
        WriteU32(output, state->tableHash); WriteU32(output, state->build); WriteU32(output, state->timestamp);
        if (state->build > 12880) { WriteU32(output, 0); WriteU32(output, 0); WriteU32(output, state->layoutHash); WriteU32(output, 0); }
    }
    else if (table.format == FileFormat::WDB3 || table.format == FileFormat::WDB4)
    {
        WriteU32(output, state->tableHash); WriteU32(output, state->build); WriteU32(output, state->timestamp);
        WriteU32(output, static_cast<std::uint32_t>(minimum)); WriteU32(output, static_cast<std::uint32_t>(maximum));
        WriteU32(output, state->locale); WriteU32(output, 0);
        if (table.format == FileFormat::WDB4) WriteU32(output, flags);
    }
    else
    {
        WriteU32(output, state->tableHash); WriteU32(output, state->layoutHash);
        WriteU32(output, static_cast<std::uint32_t>(minimum)); WriteU32(output, static_cast<std::uint32_t>(maximum));
        WriteU32(output, state->locale); WriteU32(output, 0); WriteU16(output, flags); WriteU16(output, state->idFieldIndex);
        if (table.format == FileFormat::WDB6) { WriteU32(output, static_cast<std::uint32_t>(physical.size())); WriteU32(output, 0); }
        for (auto const& field : metadata) { WriteU16(output, static_cast<std::uint16_t>(field.bits)); WriteU16(output, static_cast<std::uint16_t>(field.offset)); }
    }

    if (table.format == FileFormat::WDB3 && (flags & FlagSecondaryKey) != 0)
    {
        auto relationIndex = static_cast<std::size_t>(relation - table.columns.begin());
        std::unordered_map<std::int32_t, std::uint32_t> values;
        for (std::size_t i = 0; i < ids.size(); ++i) values[ids[i]] = static_cast<std::uint32_t>(CellBits(table.rows[i].cells[relationIndex]));
        for (std::uint64_t i = 0; i < rangeCount; ++i) WriteU32(output, values[static_cast<std::int32_t>(minimum + i)]);
    }
    for (auto const& record : records) output.write(reinterpret_cast<char const*>(record.data()), static_cast<std::streamsize>(record.size()));
    output.write(strings.data(), static_cast<std::streamsize>(strings.size()));
    if (table.format != FileFormat::WDB3 && (flags & FlagSecondaryKey) != 0)
    {
        auto relationIndex = static_cast<std::size_t>(relation - table.columns.begin());
        std::unordered_map<std::int32_t, std::uint32_t> values;
        for (std::size_t i = 0; i < ids.size(); ++i) values[ids[i]] = static_cast<std::uint32_t>(CellBits(table.rows[i].cells[relationIndex]));
        for (std::uint64_t i = 0; i < rangeCount; ++i) WriteU32(output, values[static_cast<std::int32_t>(minimum + i)]);
    }
    if ((flags & FlagIndex) != 0) for (auto id : ids) WriteU32(output, static_cast<std::uint32_t>(id));
    output.close();
    if (!output) { std::error_code ignored; std::filesystem::remove(temporary, ignored); error = "Writing the DB2 file failed."; return false; }

    std::string verifyError;
    auto reloaded = LoadWdb(temporary, table.columns, verifyError);
    bool matches = reloaded && reloaded->rows.size() == table.rows.size();
    if (matches)
    {
        for (std::size_t i = 0; i < table.rows.size() && matches; ++i)
            matches = reloaded->rows[i].cells == table.rows[i].cells;
    }
    if (!matches)
    {
        error = "The rebuilt DB2 file failed validation: " +
            (verifyError.empty() ? "decoded values changed during the round trip." : verifyError);
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        return false;
    }
    if (!ReplaceWithBackup(temporary, path, error)) { std::error_code ignored; std::filesystem::remove(temporary, ignored); return false; }
    error.clear();
    return true;
}
}
