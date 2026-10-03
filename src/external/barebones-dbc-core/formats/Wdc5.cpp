#include "formats/BinaryTable.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <unordered_map>

namespace dbc
{
namespace
{
constexpr std::size_t Wdc1HeaderSize = 84;
constexpr std::size_t WdcHeaderSize = 72;
constexpr std::size_t Wdc5HeaderSize = 204;
constexpr std::uint16_t FlagSparse = 0x1;
constexpr std::uint16_t FlagSecondaryKey = 0x2;
constexpr std::uint16_t FlagIndex = 0x4;

struct FieldMeta
{
    std::int16_t bits = 0;
    std::int16_t offset = 0;
};

struct ColumnMeta
{
    std::uint16_t recordOffset = 0;
    std::uint16_t size = 0;
    std::uint32_t additionalSize = 0;
    std::uint32_t compression = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::vector<std::uint32_t> pallet;
    std::unordered_map<std::int32_t, std::uint32_t> common;
};

struct Section
{
    std::uint64_t tactKey = 0;
    std::uint32_t fileOffset = 0;
    std::uint32_t recordCount = 0;
    std::uint32_t stringSize = 0;
    std::uint32_t recordsEnd = 0;
    std::uint32_t indexSize = 0;
    std::uint32_t parentSize = 0;
    std::uint32_t offsetCount = 0;
    std::uint32_t copyCount = 0;
    std::vector<std::int32_t> ids;
    std::vector<std::pair<std::int32_t, std::int32_t>> copies;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> sparseEntries;
    std::unordered_map<std::int32_t, std::int32_t> references;
    std::size_t previousStringSize = 0;
    bool opaque = false;
};

struct WdcState final : TableFormatState
{
    FileFormat format = FileFormat::Unknown;
    std::uint32_t schemaVersion = 0;
    std::string schemaTag;
    std::uint32_t recordSize = 0;
    std::uint32_t tableHash = 0;
    std::uint32_t layoutHash = 0;
    std::uint32_t locale = 0;
    std::uint16_t flags = 0;
    std::uint16_t idFieldIndex = 0;
    std::uint32_t packedDataOffset = 0;
    std::vector<FieldMeta> fields;
    std::vector<ColumnMeta> columns;
    bool hasOpaqueSections = false;
};

bool Range(std::vector<unsigned char> const& bytes, std::size_t offset, std::size_t size)
{
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

std::uint64_t ReadUnsigned(std::vector<unsigned char> const& bytes, std::size_t offset, std::size_t size)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index)
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
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

std::uint64_t U64(std::vector<unsigned char> const& bytes, std::size_t offset)
{
    return ReadUnsigned(bytes, offset, 8);
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

std::uint32_t StoredBits(FieldMeta const& field, ColumnMeta const& column)
{
    if (column.compression == 1 || column.compression == 3 || column.compression == 4 || column.compression == 5)
        return column.b;
    if (column.compression == 2) return 0;
    auto const width = 32 - static_cast<int>(field.bits);
    return width > 0 ? static_cast<std::uint32_t>(width) : column.b;
}

CellValue NumericCell(std::uint64_t raw, Column const& column, std::uint32_t bits, bool forceSigned)
{
    if (column.type == ValueType::Float)
        return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
    if (column.type == ValueType::UnsignedInteger)
        return raw;
    auto const signedBits = forceSigned ? bits : std::min(column.storageBits, bits == 0 ? column.storageBits : bits);
    return SignExtend(raw, signedBits);
}

std::uint64_t CellBits(CellValue const& cell)
{
    if (auto value = std::get_if<std::int64_t>(&cell)) return static_cast<std::uint64_t>(*value);
    if (auto value = std::get_if<std::uint64_t>(&cell)) return *value;
    if (auto value = std::get_if<double>(&cell)) return std::bit_cast<std::uint32_t>(static_cast<float>(*value));
    return 0;
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

void WriteU64(std::ostream& stream, std::uint64_t value)
{
    WriteU32(stream, static_cast<std::uint32_t>(value));
    WriteU32(stream, static_cast<std::uint32_t>(value >> 32));
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

bool Fits(CellValue const& value, Column const& column, std::uint32_t bits)
{
    if (column.type == ValueType::Float || column.type == ValueType::String || bits >= 64) return true;
    if (column.type == ValueType::UnsignedInteger)
    {
        auto item = std::get_if<std::uint64_t>(&value);
        return item && *item <= ((std::uint64_t { 1 } << bits) - 1);
    }
    auto item = std::get_if<std::int64_t>(&value);
    if (!item || bits == 0) return false;
    auto const minimum = -(std::int64_t { 1 } << (bits - 1));
    auto const maximum = (std::int64_t { 1 } << (bits - 1)) - 1;
    return *item >= minimum && *item <= maximum;
}

std::string PaletteKey(std::vector<std::uint32_t> const& values)
{
    std::string key(values.size() * 4, '\0');
    for (std::size_t i = 0; i < values.size(); ++i)
        std::memcpy(key.data() + i * 4, &values[i], 4);
    return key;
}

std::optional<std::int32_t> RowId(Row const& row, std::vector<Column> const& columns)
{
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (!columns[i].id) continue;
        if (auto value = std::get_if<std::int64_t>(&row.cells[i]))
        {
            if (*value < std::numeric_limits<std::int32_t>::min() || *value > std::numeric_limits<std::int32_t>::max()) return {};
            return static_cast<std::int32_t>(*value);
        }
        if (auto value = std::get_if<std::uint64_t>(&row.cells[i]))
        {
            if (*value > std::numeric_limits<std::uint32_t>::max()) return {};
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(*value));
        }
    }
    return {};
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
        if (fsError)
        {
            error = "Could not create backup: " + fsError.message();
            std::filesystem::remove(temporary, fsError);
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, fsError);
    if (fsError)
    {
        std::filesystem::remove(destination, fsError);
        fsError.clear();
        std::filesystem::rename(temporary, destination, fsError);
    }
    if (fsError)
    {
        error = "Could not replace destination: " + fsError.message();
        return false;
    }
    return true;
}
}

std::optional<Table> BinaryTableCodec::LoadWdc(std::filesystem::path const& path,
    std::vector<Column> columns, std::string& error)
{
    auto fail = [&](std::string message) -> std::optional<Table> { error = std::move(message); return std::nullopt; };
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return fail("Could not open " + path.string());
    auto const length = stream.tellg();
    if (length < static_cast<std::streamoff>(WdcHeaderSize)) return fail("The WDC file is truncated.");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!stream) return fail("Could not read " + path.string());

    auto state = std::make_shared<WdcState>();
    if (std::memcmp(bytes.data(), "WDC1", 4) == 0) state->format = FileFormat::WDC1;
    else if (std::memcmp(bytes.data(), "WDC2", 4) == 0) state->format = FileFormat::WDC2;
    else if (std::memcmp(bytes.data(), "WDC3", 4) == 0) state->format = FileFormat::WDC3;
    else if (std::memcmp(bytes.data(), "WDC4", 4) == 0) state->format = FileFormat::WDC4;
    else if (std::memcmp(bytes.data(), "WDC5", 4) == 0) state->format = FileFormat::WDC5;
    else return fail("The file is not a WDC1-WDC5 table.");

    std::uint32_t recordCount = 0, fieldCount = 0, minId = 0, sectionCount = 0;
    std::size_t headerSize = state->format == FileFormat::WDC5 ? Wdc5HeaderSize :
        state->format == FileFormat::WDC1 ? Wdc1HeaderSize : WdcHeaderSize;
    if (!Range(bytes, 0, headerSize)) return fail("The WDC header is truncated.");
    if (state->format == FileFormat::WDC5)
    {
        state->schemaVersion = U32(bytes, 4);
        auto tagEnd = std::find(bytes.begin() + 8, bytes.begin() + 136, 0);
        state->schemaTag.assign(reinterpret_cast<char const*>(bytes.data() + 8), static_cast<std::size_t>(tagEnd - (bytes.begin() + 8)));
        recordCount = U32(bytes, 136); fieldCount = U32(bytes, 140); state->recordSize = U32(bytes, 144);
        state->tableHash = U32(bytes, 152); state->layoutHash = U32(bytes, 156); minId = U32(bytes, 160);
        state->locale = U32(bytes, 168); state->flags = U16(bytes, 172); state->idFieldIndex = U16(bytes, 174);
        state->packedDataOffset = U32(bytes, 180); sectionCount = U32(bytes, 200);
    }
    else
    {
        recordCount = U32(bytes, 4); fieldCount = U32(bytes, 8); state->recordSize = U32(bytes, 12);
        state->tableHash = U32(bytes, 20); state->layoutHash = U32(bytes, 24); minId = U32(bytes, 28);
        state->locale = U32(bytes, 36);
        auto const flagsOffset = state->format == FileFormat::WDC1 ? 44u : 40u;
        state->flags = U16(bytes, flagsOffset); state->idFieldIndex = U16(bytes, flagsOffset + 2);
        state->packedDataOffset = U32(bytes, flagsOffset + 8);
        sectionCount = state->format == FileFormat::WDC1 ? (recordCount == 0 ? 0u : 1u) : U32(bytes, 68);
    }

    auto const sectionHeaderSize = state->format == FileFormat::WDC2 ? 36u : 40u;
    auto const sectionHeadersEnd = headerSize + (state->format == FileFormat::WDC1 ? 0u : static_cast<std::size_t>(sectionCount) * sectionHeaderSize);
    auto const fieldMetaOffset = sectionHeadersEnd;
    std::size_t columnMetaOffset = fieldMetaOffset + static_cast<std::size_t>(fieldCount) * 4;
    std::uint32_t wdc1ReferenceSize = 0;
    std::vector<Section> sections(sectionCount);
    if (state->format == FileFormat::WDC1 && sectionCount != 0)
    {
        auto& section = sections[0];
        auto const stringSize = U32(bytes, 16);
        auto const maxId = U32(bytes, 32);
        auto const copySize = U32(bytes, 40);
        auto const sparseOffset = U32(bytes, 60);
        section.fileOffset = static_cast<std::uint32_t>(fieldMetaOffset + static_cast<std::size_t>(fieldCount) * 4);
        section.recordCount = recordCount; section.stringSize = stringSize; section.indexSize = U32(bytes, 64);
        section.parentSize = U32(bytes, 80); wdc1ReferenceSize = section.parentSize;
        if ((state->flags & FlagSparse) != 0)
        {
            section.recordsEnd = sparseOffset;
            section.offsetCount = maxId >= minId ? maxId - minId + 1 : 0;
            columnMetaOffset = static_cast<std::size_t>(sparseOffset) + static_cast<std::size_t>(section.offsetCount) * 6 + section.indexSize;
        }
        else
        {
            section.recordsEnd = section.fileOffset + recordCount * state->recordSize;
            section.copyCount = copySize / 8;
            columnMetaOffset = static_cast<std::size_t>(section.recordsEnd) + stringSize + section.indexSize + copySize;
        }
    }
    else
    {
        for (std::uint32_t i = 0; i < sectionCount; ++i)
        {
            auto const offset = headerSize + static_cast<std::size_t>(i) * sectionHeaderSize;
            auto& section = sections[i];
            section.tactKey = U64(bytes, offset);
            section.fileOffset = U32(bytes, offset + 8);
            section.recordCount = U32(bytes, offset + 12);
            section.stringSize = U32(bytes, offset + 16);
            if (state->format == FileFormat::WDC2)
            {
                section.copyCount = U32(bytes, offset + 20) / 8;
                section.recordsEnd = U32(bytes, offset + 24);
                section.indexSize = U32(bytes, offset + 28);
                section.parentSize = U32(bytes, offset + 32);
                if ((state->flags & FlagSparse) != 0)
                {
                    auto const maxId = U32(bytes, 32);
                    section.offsetCount = maxId >= minId ? maxId - minId + 1 : 0;
                }
            }
            else
            {
                section.recordsEnd = U32(bytes, offset + 20);
                section.indexSize = U32(bytes, offset + 24);
                section.parentSize = U32(bytes, offset + 28);
                section.offsetCount = U32(bytes, offset + 32);
                section.copyCount = U32(bytes, offset + 36);
            }
        }
    }
    if (sectionCount == 0 || recordCount == 0)
    {
        if (!Range(bytes, fieldMetaOffset, static_cast<std::size_t>(fieldCount) * 4))
            return fail("The empty WDC file has truncated field metadata.");
        state->fields.resize(fieldCount);
        for (std::uint32_t i = 0; i < fieldCount; ++i)
        {
            state->fields[i].bits = static_cast<std::int16_t>(U16(bytes, fieldMetaOffset + i * 4));
            state->fields[i].offset = static_cast<std::int16_t>(U16(bytes, fieldMetaOffset + i * 4 + 2));
        }
        Table table;
        table.path = path;
        table.format = state->format;
        table.columns = std::move(columns);
        table.formatState = state;
        table.writable = false;
        table.readOnlyReason = "This empty WDC file omits the column storage metadata required to create its first row.";
        error.clear();
        return table;
    }

    auto const mappedFields = std::accumulate(columns.begin(), columns.end(), std::uint32_t { 0 },
        [](std::uint32_t count, Column const& column) {
            return column.nonInline || column.arrayIndex != 0 ? count : count + 1;
        });
    if (mappedFields != fieldCount)
        return fail("The selected definition maps " + std::to_string(mappedFields) +
            " physical fields, but this WDC file contains " + std::to_string(fieldCount) + ".");

    if (!Range(bytes, columnMetaOffset, static_cast<std::size_t>(fieldCount) * 24))
        return fail("The WDC metadata is truncated.");

    state->fields.resize(fieldCount);
    state->columns.resize(fieldCount);
    for (std::uint32_t i = 0; i < fieldCount; ++i)
    {
        state->fields[i].bits = static_cast<std::int16_t>(U16(bytes, fieldMetaOffset + i * 4));
        state->fields[i].offset = static_cast<std::int16_t>(U16(bytes, fieldMetaOffset + i * 4 + 2));
        auto const offset = columnMetaOffset + static_cast<std::size_t>(i) * 24;
        auto& column = state->columns[i];
        column.recordOffset = U16(bytes, offset);
        column.size = U16(bytes, offset + 2);
        column.additionalSize = U32(bytes, offset + 4);
        column.compression = U32(bytes, offset + 8);
        column.a = U32(bytes, offset + 12);
        column.b = U32(bytes, offset + 16);
        column.c = U32(bytes, offset + 20);
        if (column.compression > 5) return fail("WDC5 column " + std::to_string(i) + " uses an unknown compression type.");
    }

    auto cursor = columnMetaOffset + static_cast<std::size_t>(fieldCount) * 24;
    for (auto& column : state->columns)
    {
        if (column.compression != 3) continue;
        if (!Range(bytes, cursor, column.additionalSize) || column.additionalSize % 4 != 0) return fail("WDC5 pallet data is truncated.");
        for (std::uint32_t i = 0; i < column.additionalSize / 4; ++i) column.pallet.push_back(U32(bytes, cursor + i * 4));
        cursor += column.additionalSize;
    }
    if (state->format == FileFormat::WDC1 && wdc1ReferenceSize > 0)
    {
        if (!Range(bytes, cursor, wdc1ReferenceSize) || wdc1ReferenceSize < 12)
            return fail("The WDC1 relationship data is truncated.");
        auto const count = U32(bytes, cursor);
        if (12u + static_cast<std::uint64_t>(count) * 8u > wdc1ReferenceSize)
            return fail("The WDC1 relationship data has an invalid count.");
        for (std::uint32_t i = 0; i < count; ++i)
            sections[0].references[static_cast<std::int32_t>(U32(bytes, cursor + 12 + i * 8 + 4))] =
                static_cast<std::int32_t>(U32(bytes, cursor + 12 + i * 8));
        cursor += wdc1ReferenceSize;
    }
    for (auto& column : state->columns)
    {
        if (column.compression != 4) continue;
        if (!Range(bytes, cursor, column.additionalSize) || column.additionalSize % 4 != 0) return fail("WDC5 pallet-array data is truncated.");
        for (std::uint32_t i = 0; i < column.additionalSize / 4; ++i) column.pallet.push_back(U32(bytes, cursor + i * 4));
        cursor += column.additionalSize;
    }
    for (auto& column : state->columns)
    {
        if (column.compression != 2) continue;
        if (!Range(bytes, cursor, column.additionalSize) || column.additionalSize % 8 != 0) return fail("WDC5 common data is truncated.");
        for (std::uint32_t i = 0; i < column.additionalSize / 8; ++i)
            column.common[static_cast<std::int32_t>(U32(bytes, cursor + i * 8))] = U32(bytes, cursor + i * 8 + 4);
        cursor += column.additionalSize;
    }
    // WDC4 and WDC5 store encrypted row-ID lists between metadata and payloads.
    for (auto const& section : sections)
    {
        if (state->format < FileFormat::WDC4 || section.tactKey == 0) continue;
        if (!Range(bytes, cursor, 4)) return fail("The encrypted-ID list is truncated.");
        auto const count = U32(bytes, cursor);
        cursor += 4;
        if (!Range(bytes, cursor, static_cast<std::size_t>(count) * 4)) return fail("The encrypted-ID list is truncated.");
        cursor += static_cast<std::size_t>(count) * 4;
    }

    std::vector<unsigned char> globalStrings;
    std::uint32_t previousRecordCount = 0;
    for (auto& section : sections)
    {
        section.previousStringSize = globalStrings.size();
        auto const sparse = (state->flags & FlagSparse) != 0;
        auto const recordBytes = sparse ? static_cast<std::size_t>(section.recordsEnd - section.fileOffset)
                                        : static_cast<std::size_t>(section.recordCount) * state->recordSize;
        auto const stringStart = static_cast<std::size_t>(section.fileOffset) + recordBytes;
        if (!Range(bytes, section.fileOffset, recordBytes) || (!sparse && !Range(bytes, stringStart, section.stringSize)))
            return fail("A WDC section payload is truncated.");
        if (!sparse)
            globalStrings.insert(globalStrings.end(), bytes.begin() + stringStart, bytes.begin() + stringStart + section.stringSize);

        auto aux = stringStart + (sparse ? 0 : section.stringSize);
        bool allZero = section.tactKey != 0 && std::all_of(bytes.begin() + section.fileOffset,
            bytes.begin() + section.fileOffset + recordBytes, [](unsigned char value) { return value == 0; });
        if (allZero)
        {
            if (section.indexSize > 0 || section.copyCount > 0) allZero = Range(bytes, aux, 4) && U32(bytes, aux) == 0;
            else if (section.offsetCount > 0) allZero = Range(bytes, aux, 6) && U16(bytes, aux + 4) == 0;
        }
        section.opaque = allZero;
        state->hasOpaqueSections |= allZero;
        if (section.opaque) { previousRecordCount += section.recordCount; continue; }

        auto readRangeSparseMap = [&]() -> bool {
            if (!Range(bytes, aux, static_cast<std::size_t>(section.offsetCount) * 6)) return false;
            std::unordered_map<std::uint64_t, std::int32_t> seen;
            for (std::uint32_t i = 0; i < section.offsetCount; ++i)
            {
                auto const offset = U32(bytes, aux + i * 6);
                auto const size = U16(bytes, aux + i * 6 + 4);
                if (offset == 0 || size == 0) continue;
                auto const id = static_cast<std::int32_t>(minId + i);
                auto const key = (static_cast<std::uint64_t>(offset) << 16) | size;
                if (auto found = seen.find(key); found != seen.end()) section.copies.emplace_back(id, found->second);
                else { seen[key] = id; section.sparseEntries.emplace_back(offset, size); section.ids.push_back(id); }
            }
            aux += static_cast<std::size_t>(section.offsetCount) * 6;
            return true;
        };

        if ((state->format == FileFormat::WDC1 || state->format == FileFormat::WDC2) && section.offsetCount > 0)
            if (!readRangeSparseMap()) return fail("A WDC sparse offset map is truncated.");

        if (!Range(bytes, aux, section.indexSize)) return fail("A WDC index table is truncated.");
        if (section.indexSize > 0)
        {
            section.ids.clear();
            for (std::uint32_t i = 0; i < section.indexSize / 4; ++i)
                section.ids.push_back(static_cast<std::int32_t>(U32(bytes, aux + i * 4)));
        }
        aux += section.indexSize;
        if (!section.ids.empty() && std::all_of(section.ids.begin(), section.ids.end(), [](auto id) { return id == 0; }))
            for (std::uint32_t i = 0; i < section.recordCount; ++i) section.ids[i] = static_cast<std::int32_t>(minId + previousRecordCount + i);

        if (!Range(bytes, aux, static_cast<std::size_t>(section.copyCount) * 8)) return fail("A WDC copy table is truncated.");
        for (std::uint32_t i = 0; i < section.copyCount; ++i)
            section.copies.emplace_back(static_cast<std::int32_t>(U32(bytes, aux + i * 8)),
                static_cast<std::int32_t>(U32(bytes, aux + i * 8 + 4)));
        aux += static_cast<std::size_t>(section.copyCount) * 8;

        if (state->format >= FileFormat::WDC3)
        {
            if (!Range(bytes, aux, static_cast<std::size_t>(section.offsetCount) * 6)) return fail("A WDC sparse offset map is truncated.");
            for (std::uint32_t i = 0; i < section.offsetCount; ++i)
                section.sparseEntries.emplace_back(U32(bytes, aux + i * 6), U16(bytes, aux + i * 6 + 4));
            aux += static_cast<std::size_t>(section.offsetCount) * 6;
        }

        if (section.offsetCount > 0 && (state->flags & FlagSecondaryKey) != 0)
        {
            if (state->format < FileFormat::WDC3) { /* range-map IDs were populated above */ }
            else if (!Range(bytes, aux, static_cast<std::size_t>(section.offsetCount) * 4)) return fail("A WDC sparse ID list is truncated.");
            if (state->format < FileFormat::WDC3) { }
            else
            {
            section.ids.clear();
            for (std::uint32_t i = 0; i < section.offsetCount; ++i) section.ids.push_back(static_cast<std::int32_t>(U32(bytes, aux + i * 4)));
            aux += static_cast<std::size_t>(section.offsetCount) * 4;
            }
        }
        if (section.parentSize > 0 && state->format != FileFormat::WDC1)
        {
            if (!Range(bytes, aux, section.parentSize) || section.parentSize < 12) return fail("WDC relationship data is truncated.");
            auto const count = U32(bytes, aux);
            if (12u + static_cast<std::uint64_t>(count) * 8u > section.parentSize) return fail("WDC relationship data has an invalid count.");
            for (std::uint32_t i = 0; i < count; ++i)
                section.references[static_cast<std::int32_t>(U32(bytes, aux + 12 + i * 8 + 4))] =
                    static_cast<std::int32_t>(U32(bytes, aux + 12 + i * 8));
            aux += section.parentSize;
        }
        if (section.offsetCount > 0 && (state->flags & FlagSecondaryKey) == 0 && state->format >= FileFormat::WDC3)
        {
            if (!Range(bytes, aux, static_cast<std::size_t>(section.offsetCount) * 4)) return fail("A WDC sparse ID list is truncated.");
            section.ids.clear();
            for (std::uint32_t i = 0; i < section.offsetCount; ++i) section.ids.push_back(static_cast<std::int32_t>(U32(bytes, aux + i * 4)));
        }
        previousRecordCount += section.recordCount;
    }

    Table table;
    table.path = path;
    table.format = state->format;
    table.columns = std::move(columns);
    table.formatState = state;
    if (state->hasOpaqueSections)
    {
        table.writable = false;
        table.readOnlyReason = "This file contains zero-filled encrypted sections. Their rows cannot be reconstructed without the matching TACT keys.";
    }

    std::unordered_map<std::int32_t, std::size_t> rowsById;
    previousRecordCount = 0;
    for (auto const& section : sections)
    {
        if (section.opaque) { previousRecordCount += section.recordCount; continue; }
        auto const sectionRecordBytes = (state->flags & FlagSparse) != 0
            ? static_cast<std::size_t>(section.recordsEnd - section.fileOffset)
            : static_cast<std::size_t>(section.recordCount) * state->recordSize;
        auto const sectionStringStart = static_cast<std::size_t>(section.fileOffset) + sectionRecordBytes;
        std::size_t sparsePosition = 0;
        for (std::uint32_t record = 0; record < section.recordCount; ++record)
        {
            auto recordOffset = static_cast<std::size_t>(section.fileOffset) +
                (((state->flags & FlagSparse) != 0) ? sparsePosition : static_cast<std::size_t>(record) * state->recordSize);
            auto const recordLimit = ((state->flags & FlagSparse) != 0)
                ? recordOffset + (record < section.sparseEntries.size() ? section.sparseEntries[record].second : 0)
                : recordOffset + state->recordSize;
            if (recordLimit > bytes.size()) return fail("A WDC record extends past the end of the file.");

            std::int32_t id = -1;
            if (!section.ids.empty() && record < section.ids.size()) id = section.ids[record];
            if (id == -1)
            {
                auto idColumn = std::find_if(table.columns.begin(), table.columns.end(), [](Column const& column) { return column.id && !column.nonInline; });
                if (idColumn == table.columns.end()) return fail("The WDC file has neither an index table nor an inline ID field.");
                auto const physical = idColumn->physicalField;
                auto const bits = StoredBits(state->fields[physical], state->columns[physical]);
                auto const bitPosition = (state->flags & FlagSparse) != 0 ? state->columns[physical].recordOffset : state->columns[physical].recordOffset;
                id = static_cast<std::int32_t>(ReadBits(bytes, recordOffset, bitPosition, bits));
            }

            Row row;
            row.stableId = table.nextStableId++;
            row.cells.resize(table.columns.size());
            std::size_t bitCursor = 0;
            for (std::uint32_t physical = 0; physical < fieldCount; ++physical)
            {
                auto const first = std::find_if(table.columns.begin(), table.columns.end(), [&](Column const& column) {
                    return column.physicalField == physical && column.arrayIndex == 0;
                });
                if (first == table.columns.end()) return fail("The WDC definition does not map physical field " + std::to_string(physical) + ".");
                auto const firstIndex = static_cast<std::size_t>(first - table.columns.begin());
                auto const& meta = state->columns[physical];
                auto const bits = StoredBits(state->fields[physical], meta);
                auto const fieldStart = bitCursor;
                auto rawFor = [&](std::uint32_t arrayIndex) -> std::optional<std::uint64_t> {
                    if (meta.compression == 2)
                    {
                        auto found = meta.common.find(id);
                        return found == meta.common.end() ? meta.a : found->second;
                    }
                    if (meta.compression == 3 || meta.compression == 4)
                    {
                        auto const index = static_cast<std::size_t>(ReadBits(bytes, recordOffset, fieldStart, bits));
                        auto const cardinality = meta.compression == 4 ? first->arraySize : 1u;
                        auto const palletIndex = index * cardinality + arrayIndex;
                        if (palletIndex >= meta.pallet.size()) return std::nullopt;
                        return meta.pallet[palletIndex];
                    }
                    return ReadBits(bytes, recordOffset, fieldStart + static_cast<std::size_t>(arrayIndex) * bits, bits);
                };
                for (std::uint32_t arrayIndex = 0; arrayIndex < first->arraySize; ++arrayIndex)
                {
                    auto const columnIndex = firstIndex + arrayIndex;
                    if (table.columns[columnIndex].type == ValueType::String)
                    {
                        if ((state->flags & FlagSparse) != 0)
                        {
                            auto const stringOffset = recordOffset + (fieldStart + static_cast<std::size_t>(arrayIndex) * bits) / 8;
                            auto value = ReadCString(bytes, stringOffset, recordLimit);
                            row.cells[columnIndex] = value;
                            bitCursor = (stringOffset - recordOffset + value.size() + 1) * 8;
                        }
                        else
                        {
                            auto const raw = rawFor(arrayIndex);
                            if (!raw) return fail("A WDC pallet index is outside its pallet data.");
                            auto const signedOffset = SignExtend(*raw, bits);
                            std::int64_t globalKey = 0;
                            if (state->format == FileFormat::WDC1)
                                globalKey = static_cast<std::int64_t>(*raw);
                            else if (state->format == FileFormat::WDC2)
                                globalKey = static_cast<std::int64_t>(section.previousStringSize) +
                                    static_cast<std::int64_t>(recordOffset + (fieldStart + static_cast<std::size_t>(arrayIndex) * bits) / 8) +
                                    signedOffset - static_cast<std::int64_t>(sectionStringStart);
                            else
                                globalKey = static_cast<std::int64_t>((previousRecordCount + record) * state->recordSize) -
                                    static_cast<std::int64_t>(recordCount * state->recordSize) +
                                    static_cast<std::int64_t>((fieldStart + static_cast<std::size_t>(arrayIndex) * bits) / 8) + signedOffset;
                            if (globalKey < 0 || static_cast<std::size_t>(globalKey) >= globalStrings.size()) row.cells[columnIndex] = std::string {};
                            else row.cells[columnIndex] = ReadCString(globalStrings, static_cast<std::size_t>(globalKey), globalStrings.size());
                        }
                    }
                    else
                    {
                        auto const raw = rawFor(arrayIndex);
                        if (!raw) return fail("A WDC pallet index is outside its pallet data.");
                        auto const valueBits = meta.compression == 2 || meta.compression == 3 || meta.compression == 4
                            ? table.columns[columnIndex].storageBits : bits;
                        row.cells[columnIndex] = NumericCell(*raw, table.columns[columnIndex], valueBits, meta.compression == 5);
                    }
                }
                if (meta.compression != 2 && !((state->flags & FlagSparse) != 0 && first->type == ValueType::String))
                    bitCursor += (meta.compression == 4 ? bits : static_cast<std::size_t>(bits) * first->arraySize);
            }
            for (std::size_t columnIndex = 0; columnIndex < table.columns.size(); ++columnIndex)
            {
                auto const& column = table.columns[columnIndex];
                if (!column.nonInline) continue;
                if (column.id) row.cells[columnIndex] = column.type == ValueType::UnsignedInteger
                    ? CellValue { static_cast<std::uint64_t>(static_cast<std::uint32_t>(id)) }
                    : CellValue { static_cast<std::int64_t>(id) };
                else if (column.relation)
                {
                    auto const key = (state->flags & FlagSecondaryKey) != 0 ? id : static_cast<std::int32_t>(record);
                    auto found = section.references.find(key);
                    auto const value = found == section.references.end() ? 0 : found->second;
                    row.cells[columnIndex] = column.type == ValueType::UnsignedInteger
                        ? CellValue { static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)) }
                        : CellValue { static_cast<std::int64_t>(value) };
                }
            }
            rowsById[id] = table.rows.size();
            table.rows.push_back(std::move(row));
            if ((state->flags & FlagSparse) != 0 && record < section.sparseEntries.size()) sparsePosition += section.sparseEntries[record].second;
        }
        previousRecordCount += section.recordCount;
    }
    for (auto const& section : sections)
    {
        for (auto const& [destination, source] : section.copies)
        {
            auto found = rowsById.find(source);
            if (found == rowsById.end()) continue;
            Row row = table.rows[found->second];
            row.stableId = table.nextStableId++;
            for (std::size_t i = 0; i < table.columns.size(); ++i)
                if (table.columns[i].id)
                    row.cells[i] = table.columns[i].type == ValueType::UnsignedInteger
                        ? CellValue { static_cast<std::uint64_t>(static_cast<std::uint32_t>(destination)) }
                        : CellValue { static_cast<std::int64_t>(destination) };
            rowsById[destination] = table.rows.size();
            table.rows.push_back(std::move(row));
        }
    }
    error.clear();
    return table;
}

bool BinaryTableCodec::SaveWdc(Table const& table, std::filesystem::path const& path, std::string& error)
{
    auto state = std::dynamic_pointer_cast<WdcState>(table.formatState);
    if (!state || state->format != table.format) { error = "The WDC editing state is missing; reopen the source file before saving."; return false; }
    if (!table.writable || state->hasOpaqueSections)
    {
        error = table.readOnlyReason.empty() ? "This WDC table contains opaque encrypted rows and cannot be saved safely." : table.readOnlyReason;
        return false;
    }
    if (table.rows.size() > std::numeric_limits<std::uint32_t>::max()) { error = "The table contains too many rows."; return false; }

    std::vector<std::int32_t> ids;
    ids.reserve(table.rows.size());
    std::unordered_map<std::int32_t, bool> uniqueIds;
    for (auto const& row : table.rows)
    {
        auto id = RowId(row, table.columns);
        if (!id) { error = "Every WDC row needs an ID that fits in 32 bits."; return false; }
        if (!uniqueIds.emplace(*id, true).second) { error = "Row ID " + std::to_string(*id) + " is duplicated."; return false; }
        ids.push_back(*id);
    }

    auto metadata = state->columns;
    std::vector<std::vector<std::vector<std::uint32_t>>> palettes(metadata.size());
    std::vector<std::unordered_map<std::string, std::uint32_t>> paletteIndexes(metadata.size());
    std::vector<std::map<std::int32_t, std::uint32_t>> common(metadata.size());
    bool hasRelation = false;
    for (auto const& column : table.columns) hasRelation |= column.relation && column.nonInline;
    auto const outputFlags = static_cast<std::uint16_t>(state->flags & ~FlagSparse);

    for (std::size_t physical = 0; physical < metadata.size(); ++physical)
    {
        auto first = std::find_if(table.columns.begin(), table.columns.end(), [&](Column const& column) {
            return column.physicalField == physical && column.arrayIndex == 0;
        });
        if (first == table.columns.end()) { error = "The definition no longer maps every WDC field."; return false; }
        auto const firstIndex = static_cast<std::size_t>(first - table.columns.begin());
        auto& meta = metadata[physical];
        if (meta.compression == 2)
        {
            for (std::size_t rowIndex = 0; rowIndex < table.rows.size(); ++rowIndex)
            {
                auto const raw = static_cast<std::uint32_t>(CellBits(table.rows[rowIndex].cells[firstIndex]));
                if (raw != meta.a) common[physical][ids[rowIndex]] = raw;
            }
            meta.additionalSize = static_cast<std::uint32_t>(common[physical].size() * 8);
        }
        else if (meta.compression == 3 || meta.compression == 4)
        {
            for (auto const& row : table.rows)
            {
                std::vector<std::uint32_t> values;
                auto const cardinality = meta.compression == 4 ? first->arraySize : 1u;
                for (std::uint32_t i = 0; i < cardinality; ++i)
                    values.push_back(static_cast<std::uint32_t>(CellBits(row.cells[firstIndex + i])));
                auto key = PaletteKey(values);
                if (!paletteIndexes[physical].contains(key))
                {
                    auto const index = static_cast<std::uint32_t>(palettes[physical].size());
                    paletteIndexes[physical].emplace(std::move(key), index);
                    palettes[physical].push_back(std::move(values));
                }
            }
            auto const bitWidth = meta.b;
            auto const capacity = bitWidth >= 32 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t { 1 } << bitWidth);
            if (palettes[physical].size() > capacity)
            {
                error = "Column '" + first->name + "' now needs more pallet entries than its " + std::to_string(bitWidth) + "-bit index can address.";
                return false;
            }
            std::uint64_t size = 0;
            for (auto const& entry : palettes[physical]) size += entry.size() * 4;
            if (size > std::numeric_limits<std::uint32_t>::max()) { error = "WDC pallet data is too large."; return false; }
            meta.additionalSize = static_cast<std::uint32_t>(size);
        }
    }

    std::vector<unsigned char> strings { 0 };
    std::unordered_map<std::string, std::uint32_t> stringOffsets { { "", 0 } };
    if ((outputFlags & FlagSparse) == 0)
    {
        for (auto const& row : table.rows)
            for (std::size_t i = 0; i < table.columns.size(); ++i)
                if (table.columns[i].type == ValueType::String)
                {
                    auto const& value = std::get<std::string>(row.cells[i]);
                    if (stringOffsets.contains(value)) continue;
                    if (strings.size() + value.size() + 1 > std::numeric_limits<std::uint32_t>::max()) { error = "The WDC string table is too large."; return false; }
                    stringOffsets[value] = static_cast<std::uint32_t>(strings.size());
                    strings.insert(strings.end(), value.begin(), value.end());
                    strings.push_back(0);
                }
    }

    std::vector<std::vector<unsigned char>> records;
    records.reserve(table.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < table.rows.size(); ++rowIndex)
    {
        std::vector<unsigned char> record;
        if ((outputFlags & FlagSparse) == 0) record.resize(state->recordSize);
        std::size_t bitCursor = 0;
        for (std::size_t physical = 0; physical < metadata.size(); ++physical)
        {
            auto first = std::find_if(table.columns.begin(), table.columns.end(), [&](Column const& column) {
                return column.physicalField == physical && column.arrayIndex == 0;
            });
            auto const firstIndex = static_cast<std::size_t>(first - table.columns.begin());
            auto const& meta = metadata[physical];
            auto const bits = StoredBits(state->fields[physical], meta);
            if (meta.compression == 2) continue;
            if (meta.compression == 3 || meta.compression == 4)
            {
                std::vector<std::uint32_t> values;
                auto const cardinality = meta.compression == 4 ? first->arraySize : 1u;
                for (std::uint32_t i = 0; i < cardinality; ++i)
                    values.push_back(static_cast<std::uint32_t>(CellBits(table.rows[rowIndex].cells[firstIndex + i])));
                auto found = paletteIndexes[physical].find(PaletteKey(values));
                WriteBits(record, bitCursor, found->second, bits);
                bitCursor += bits;
                continue;
            }
            for (std::uint32_t arrayIndex = 0; arrayIndex < first->arraySize; ++arrayIndex)
            {
                auto const columnIndex = firstIndex + arrayIndex;
                auto const& column = table.columns[columnIndex];
                auto const& cell = table.rows[rowIndex].cells[columnIndex];
                if (column.type == ValueType::String)
                {
                    auto const& value = std::get<std::string>(cell);
                    if ((outputFlags & FlagSparse) != 0)
                    {
                        if ((bitCursor & 7u) != 0) { error = "Sparse WDC strings are not byte-aligned."; return false; }
                        auto const byteCursor = bitCursor / 8;
                        if (record.size() < byteCursor) record.resize(byteCursor);
                        record.insert(record.end(), value.begin(), value.end());
                        record.push_back(0);
                        bitCursor = record.size() * 8;
                    }
                    else
                    {
                        auto const stored = state->format == FileFormat::WDC1
                            ? static_cast<std::int64_t>(stringOffsets[value])
                            : static_cast<std::int64_t>(stringOffsets[value]) +
                                static_cast<std::int64_t>((table.rows.size() - rowIndex) * state->recordSize) -
                                static_cast<std::int64_t>(bitCursor / 8);
                        WriteBits(record, bitCursor, static_cast<std::uint64_t>(stored), bits);
                        bitCursor += bits;
                    }
                }
                else
                {
                    if (!Fits(cell, column, bits) && meta.compression != 5)
                    {
                        error = "Value in column '" + column.name + "' does not fit its " + std::to_string(bits) + "-bit WDC field.";
                        return false;
                    }
                    WriteBits(record, bitCursor, CellBits(cell), bits);
                    bitCursor += bits;
                }
            }
        }
        if ((outputFlags & FlagSparse) == 0)
        {
            if (bitCursor > static_cast<std::size_t>(state->recordSize) * 8) { error = "Serialized WDC row exceeds the declared record size."; return false; }
            record.resize(state->recordSize);
        }
        else
        {
            record.resize((record.size() + 3) & ~std::size_t { 3 });
            if (record.size() > std::numeric_limits<std::uint16_t>::max()) { error = "A sparse WDC row exceeds 65535 bytes."; return false; }
        }
        records.push_back(std::move(record));
    }

    std::uint64_t palletSize = 0, commonSize = 0;
    for (auto const& meta : metadata)
    {
        if (meta.compression == 2) commonSize += meta.additionalSize;
        if (meta.compression == 3 || meta.compression == 4) palletSize += meta.additionalSize;
    }
    auto const rowCount = static_cast<std::uint32_t>(records.size());
    auto const sectionCount = rowCount == 0 ? 0u : 1u;
    auto const stringSize = static_cast<std::uint32_t>(strings.size());
    auto const indexSize = (outputFlags & FlagIndex) != 0 ? rowCount * 4u : 0u;
    auto const parentSize = hasRelation && rowCount > 0 ? 12u + rowCount * 8u : 0u;
    auto const metadataSize = static_cast<std::uint64_t>(state->fields.size()) * 4 + metadata.size() * 24 + palletSize + commonSize;
    auto const outputHeaderSize = state->format == FileFormat::WDC5 ? Wdc5HeaderSize :
        state->format == FileFormat::WDC1 ? Wdc1HeaderSize : WdcHeaderSize;
    auto const outputSectionSize = state->format == FileFormat::WDC2 ? 36u : 40u;
    auto const fileOffset64 = state->format == FileFormat::WDC1
        ? static_cast<std::uint64_t>(Wdc1HeaderSize) + state->fields.size() * 4
        : static_cast<std::uint64_t>(outputHeaderSize) + static_cast<std::uint64_t>(sectionCount) * outputSectionSize + metadataSize;
    if (fileOffset64 > std::numeric_limits<std::uint32_t>::max()) { error = "WDC metadata is too large."; return false; }
    auto const fileOffset = static_cast<std::uint32_t>(fileOffset64);
    std::uint64_t recordsBytes = 0;
    for (auto const& record : records) recordsBytes += record.size();
    if (fileOffset64 + recordsBytes + stringSize + indexSize > std::numeric_limits<std::uint32_t>::max())
    { error = "WDC record data is too large."; return false; }

    std::int32_t minimum = 0, maximum = 0;
    if (!ids.empty()) { auto const bounds = std::minmax_element(ids.begin(), ids.end()); minimum = *bounds.first; maximum = *bounds.second; }

    auto temporary = path; temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { error = "Could not create a temporary WDC file beside the destination."; return false; }

    auto writeCommonHeader = [&]() {
        WriteU32(output, rowCount); WriteU32(output, static_cast<std::uint32_t>(state->fields.size()));
        WriteU32(output, state->recordSize); WriteU32(output, stringSize); WriteU32(output, state->tableHash);
        WriteU32(output, state->layoutHash); WriteU32(output, static_cast<std::uint32_t>(minimum));
        WriteU32(output, static_cast<std::uint32_t>(maximum)); WriteU32(output, state->locale);
    };
    if (state->format == FileFormat::WDC5)
    {
        output.write("WDC5", 4); WriteU32(output, state->schemaVersion);
        std::string tag = state->schemaTag.substr(0, 128); tag.resize(128, '\0'); output.write(tag.data(), 128);
        writeCommonHeader(); WriteU16(output, outputFlags); WriteU16(output, state->idFieldIndex);
        WriteU32(output, static_cast<std::uint32_t>(state->fields.size())); WriteU32(output, rowCount == 0 ? 0 : state->packedDataOffset);
        WriteU32(output, hasRelation && rowCount > 0 ? 1u : 0u);
        WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(metadata.size() * 24));
        WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(commonSize));
        WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(palletSize)); WriteU32(output, sectionCount);
    }
    else
    {
        char magic[4] { 'W', 'D', 'C', static_cast<char>('0' + static_cast<int>(state->format) - static_cast<int>(FileFormat::WDC1) + 1) };
        output.write(magic, 4); writeCommonHeader();
        if (state->format == FileFormat::WDC1) WriteU32(output, 0);
        WriteU16(output, outputFlags); WriteU16(output, state->idFieldIndex);
        WriteU32(output, static_cast<std::uint32_t>(state->fields.size())); WriteU32(output, rowCount == 0 ? 0 : state->packedDataOffset);
        WriteU32(output, hasRelation && rowCount > 0 ? 1u : 0u);
        if (state->format == FileFormat::WDC1)
        {
            WriteU32(output, 0); WriteU32(output, indexSize);
            WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(metadata.size() * 24));
            WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(commonSize));
            WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(palletSize)); WriteU32(output, parentSize);
        }
        else
        {
            WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(metadata.size() * 24));
            WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(commonSize));
            WriteU32(output, rowCount == 0 ? 0u : static_cast<std::uint32_t>(palletSize)); WriteU32(output, sectionCount);
        }
    }

    if (sectionCount != 0 && state->format != FileFormat::WDC1)
    {
        WriteU64(output, 0); WriteU32(output, fileOffset); WriteU32(output, rowCount); WriteU32(output, stringSize);
        if (state->format == FileFormat::WDC2)
        {
            WriteU32(output, 0); WriteU32(output, 0); WriteU32(output, indexSize); WriteU32(output, parentSize);
        }
        else
        {
            WriteU32(output, 0); WriteU32(output, indexSize); WriteU32(output, parentSize); WriteU32(output, 0); WriteU32(output, 0);
        }
    }
    for (auto const& field : state->fields) { WriteU16(output, static_cast<std::uint16_t>(field.bits)); WriteU16(output, static_cast<std::uint16_t>(field.offset)); }

    auto writeColumnMetadata = [&]() {
        for (auto const& meta : metadata)
        {
            WriteU16(output, meta.recordOffset); WriteU16(output, meta.size); WriteU32(output, meta.additionalSize);
            WriteU32(output, meta.compression); WriteU32(output, meta.a); WriteU32(output, meta.b); WriteU32(output, meta.c);
        }
        for (std::size_t physical = 0; physical < metadata.size(); ++physical)
            if (metadata[physical].compression == 3)
                for (auto const& entry : palettes[physical]) for (auto value : entry) WriteU32(output, value);
        for (std::size_t physical = 0; physical < metadata.size(); ++physical)
            if (metadata[physical].compression == 4)
                for (auto const& entry : palettes[physical]) for (auto value : entry) WriteU32(output, value);
        for (std::size_t physical = 0; physical < metadata.size(); ++physical)
            if (metadata[physical].compression == 2)
                for (auto const& [id, value] : common[physical]) { WriteU32(output, static_cast<std::uint32_t>(id)); WriteU32(output, value); }
    };
    auto writeRecords = [&]() {
        for (auto const& record : records) output.write(reinterpret_cast<char const*>(record.data()), static_cast<std::streamsize>(record.size()));
        output.write(reinterpret_cast<char const*>(strings.data()), static_cast<std::streamsize>(strings.size()));
        if ((outputFlags & FlagIndex) != 0) for (auto id : ids) WriteU32(output, static_cast<std::uint32_t>(id));
    };
    auto writeRelationships = [&]() {
        if (parentSize == 0) return;
        std::vector<std::int32_t> relations; relations.reserve(rowCount);
        auto relationColumn = std::find_if(table.columns.begin(), table.columns.end(), [](Column const& column) { return column.relation && column.nonInline; });
        auto const relationIndex = static_cast<std::size_t>(relationColumn - table.columns.begin());
        for (auto const& row : table.rows) relations.push_back(static_cast<std::int32_t>(CellBits(row.cells[relationIndex])));
        auto const bounds = std::minmax_element(relations.begin(), relations.end());
        WriteU32(output, rowCount); WriteU32(output, static_cast<std::uint32_t>(*bounds.first)); WriteU32(output, static_cast<std::uint32_t>(*bounds.second));
        for (std::uint32_t i = 0; i < rowCount; ++i)
        {
            WriteU32(output, static_cast<std::uint32_t>(relations[i]));
            auto const useId = state->format >= FileFormat::WDC4 && (outputFlags & FlagSecondaryKey) != 0;
            WriteU32(output, useId ? static_cast<std::uint32_t>(ids[i]) : i);
        }
    };

    if (rowCount != 0)
    {
        if (state->format == FileFormat::WDC1) { writeRecords(); writeColumnMetadata(); writeRelationships(); }
        else { writeColumnMetadata(); writeRecords(); writeRelationships(); }
    }
    output.close();
    if (!output)
    {
        error = "Failed while writing the temporary WDC file.";
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        return false;
    }
    std::string verifyError;
    auto reloaded = LoadWdc(temporary, table.columns, verifyError);
    bool matches = reloaded && reloaded->rows.size() == table.rows.size();
    if (matches)
    {
        for (std::size_t i = 0; i < table.rows.size() && matches; ++i)
            matches = reloaded->rows[i].cells == table.rows[i].cells;
    }
    if (!matches)
    {
        error = "The rebuilt WDC file failed validation: " + (verifyError.empty() ? "decoded values changed during the round trip." : verifyError);
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        return false;
    }
    if (!ReplaceWithBackup(temporary, path, error)) return false;
    error.clear();
    return true;
}

std::optional<Table> BinaryTableCodec::LoadWdc5(std::filesystem::path const& path,
    std::vector<Column> columns, std::string& error)
{
    auto table = LoadWdc(path, std::move(columns), error);
    if (table && table->format != FileFormat::WDC5)
    {
        error = "Expected a WDC5 table.";
        return std::nullopt;
    }
    return table;
}

bool BinaryTableCodec::SaveWdc5(Table const& table, std::filesystem::path const& path, std::string& error)
{
    if (table.format != FileFormat::WDC5) { error = "Expected a WDC5 table."; return false; }
    return SaveWdc(table, path, error);
}
}
