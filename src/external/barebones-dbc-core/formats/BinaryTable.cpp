#include "formats/BinaryTable.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace dbc
{
namespace
{
std::uint32_t ReadU32(std::istream& stream)
{
    unsigned char bytes[4] {};
    stream.read(reinterpret_cast<char*>(bytes), 4);
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t ReadU16(std::istream& stream)
{
    unsigned char bytes[2] {};
    stream.read(reinterpret_cast<char*>(bytes), 2);
    return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint64_t ReadUnsigned(unsigned char const* bytes, std::size_t count)
{
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < count; ++index)
        result |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    return result;
}

void WriteU32(std::ostream& stream, std::uint32_t value)
{
    unsigned char bytes[4] {
        static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)
    };
    stream.write(reinterpret_cast<char const*>(bytes), 4);
}

FileFormat MagicToFormat(char const magic[4])
{
    if (std::memcmp(magic, "WDBC", 4) == 0) return FileFormat::WDBC;
    if (std::memcmp(magic, "WDB2", 4) == 0) return FileFormat::WDB2;
    if (std::memcmp(magic, "WDB3", 4) == 0) return FileFormat::WDB3;
    if (std::memcmp(magic, "WDB4", 4) == 0) return FileFormat::WDB4;
    if (std::memcmp(magic, "WDB5", 4) == 0) return FileFormat::WDB5;
    if (std::memcmp(magic, "WDB6", 4) == 0) return FileFormat::WDB6;
    if (std::memcmp(magic, "WDC1", 4) == 0) return FileFormat::WDC1;
    if (std::memcmp(magic, "WDC2", 4) == 0) return FileFormat::WDC2;
    if (std::memcmp(magic, "WDC3", 4) == 0) return FileFormat::WDC3;
    if (std::memcmp(magic, "WDC4", 4) == 0) return FileFormat::WDC4;
    if (std::memcmp(magic, "WDC5", 4) == 0) return FileFormat::WDC5;
    if (std::memcmp(magic, "WCH7", 4) == 0) return FileFormat::WCH7;
    if (std::memcmp(magic, "WCH8", 4) == 0) return FileFormat::WCH8;
    if (std::memcmp(magic, "XFTH", 4) == 0) return FileFormat::XFTH;
    if (std::memcmp(magic, "PTCH", 4) == 0) return FileFormat::PTCH;
    return FileFormat::Unknown;
}

std::uint64_t NumericBits(CellValue const& value)
{
    if (auto item = std::get_if<std::int64_t>(&value)) return static_cast<std::uint64_t>(*item);
    if (auto item = std::get_if<std::uint64_t>(&value)) return *item;
    if (auto item = std::get_if<double>(&value)) return std::bit_cast<std::uint32_t>(static_cast<float>(*item));
    return 0;
}

std::size_t StorageBytes(Column const& column)
{
    return column.type == ValueType::String ? 4u : std::max<std::size_t>(1, (column.storageBits + 7u) / 8u);
}

void WriteUnsigned(std::ostream& stream, std::uint64_t value, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        auto const byte = static_cast<unsigned char>(value >> (index * 8));
        stream.write(reinterpret_cast<char const*>(&byte), 1);
    }
}
}

FileHeaderInfo InspectHeader(std::filesystem::path const& path, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "Could not open " + path.string();
        return {};
    }
    char magic[4] {};
    stream.read(magic, 4);
    if (stream.gcount() != 4)
    {
        error = "The file is too small to contain a DBC/DB2 header.";
        return {};
    }
    FileHeaderInfo info;
    info.format = MagicToFormat(magic);
    if (info.format == FileFormat::Unknown)
    {
        error = "Unrecognized file signature.";
        return info;
    }
    if (info.format == FileFormat::PTCH)
    {
        error.clear();
        return info;
    }
    if (info.format == FileFormat::XFTH)
    {
        info.recordCount = ReadU32(stream); // hotfix format version
        info.fieldCount = ReadU32(stream);  // client build ID
        if (!stream) error = "The hotfix cache header is truncated.";
        else error.clear();
        return info;
    }
    if (info.format == FileFormat::WCH7 || info.format == FileFormat::WCH8)
    {
        info.recordCount = ReadU32(stream);
        ReadU32(stream); // auxiliary table size
        info.fieldCount = ReadU32(stream);
        info.recordSize = ReadU32(stream);
        info.stringBlockSize = ReadU32(stream);
        info.tableHash = ReadU32(stream);
        info.layoutHash = ReadU32(stream);
        ReadU32(stream); // client build
        ReadU32(stream); // timestamp
        info.minId = ReadU32(stream);
        info.maxId = ReadU32(stream);
        ReadU32(stream); // locale
        if (!stream) error = "The ADB hotfix header is truncated.";
        else error.clear();
        return info;
    }
    if (info.format == FileFormat::WDC5)
    {
        info.schemaVersion = ReadU32(stream);
        std::array<char, 128> tag {};
        stream.read(tag.data(), static_cast<std::streamsize>(tag.size()));
        auto const tagEnd = std::find(tag.begin(), tag.end(), '\0');
        info.schemaTag.assign(tag.data(), static_cast<std::size_t>(std::distance(tag.begin(), tagEnd)));
        info.recordCount = ReadU32(stream);
        info.fieldCount = ReadU32(stream);
        info.recordSize = ReadU32(stream);
        info.stringBlockSize = ReadU32(stream);
        info.tableHash = ReadU32(stream);
        info.layoutHash = ReadU32(stream);
        info.minId = ReadU32(stream);
        info.maxId = ReadU32(stream);
        ReadU32(stream); // locale
        info.flags = ReadU16(stream);
        info.idFieldIndex = ReadU16(stream);
        info.totalFieldCount = ReadU32(stream);
        ReadU32(stream); // packed data offset
        ReadU32(stream); // lookup column count
        ReadU32(stream); // column metadata size
        ReadU32(stream); // common data size
        ReadU32(stream); // pallet data size
        info.sectionCount = ReadU32(stream);
    }
    else if (info.format == FileFormat::WDC1)
    {
        info.recordCount = ReadU32(stream);
        info.fieldCount = ReadU32(stream);
        info.recordSize = ReadU32(stream);
        info.stringBlockSize = ReadU32(stream);
        info.tableHash = ReadU32(stream);
        info.layoutHash = ReadU32(stream);
        info.minId = ReadU32(stream);
        info.maxId = ReadU32(stream);
        ReadU32(stream); // locale
        ReadU32(stream); // copy table size
        info.flags = ReadU16(stream);
        info.idFieldIndex = ReadU16(stream);
        info.totalFieldCount = ReadU32(stream);
        ReadU32(stream); // packed data offset
        ReadU32(stream); // lookup column count
        ReadU32(stream); // sparse table offset
        ReadU32(stream); // index data size
        ReadU32(stream); // column metadata size
        ReadU32(stream); // common data size
        ReadU32(stream); // pallet data size
        ReadU32(stream); // relationship data size
    }
    else if (info.format >= FileFormat::WDC2 && info.format <= FileFormat::WDC4)
    {
        info.recordCount = ReadU32(stream);
        info.fieldCount = ReadU32(stream);
        info.recordSize = ReadU32(stream);
        info.stringBlockSize = ReadU32(stream);
        info.tableHash = ReadU32(stream);
        info.layoutHash = ReadU32(stream);
        info.minId = ReadU32(stream);
        info.maxId = ReadU32(stream);
        ReadU32(stream); // locale
        info.flags = ReadU16(stream);
        info.idFieldIndex = ReadU16(stream);
        info.totalFieldCount = ReadU32(stream);
        ReadU32(stream); // packed data offset
        ReadU32(stream); // lookup column count
        ReadU32(stream); // column metadata size
        ReadU32(stream); // common data size
        ReadU32(stream); // pallet data size
        info.sectionCount = ReadU32(stream);
    }
    else
    {
        info.recordCount = ReadU32(stream);
        info.fieldCount = ReadU32(stream);
        info.recordSize = ReadU32(stream);
        info.stringBlockSize = ReadU32(stream);
        if (info.format >= FileFormat::WDB2 && info.format <= FileFormat::WDB6)
        {
            info.tableHash = ReadU32(stream);
            if (info.format == FileFormat::WDB5 || info.format == FileFormat::WDB6)
            {
                info.layoutHash = ReadU32(stream);
                info.minId = ReadU32(stream);
                info.maxId = ReadU32(stream);
                ReadU32(stream); // locale
                ReadU32(stream); // copy table size
                info.flags = ReadU16(stream);
                info.idFieldIndex = ReadU16(stream);
                if (info.format == FileFormat::WDB6) info.totalFieldCount = ReadU32(stream);
            }
        }
    }
    if (!stream)
    {
        error = "The file has a truncated header.";
        return {};
    }
    error.clear();
    return info;
}

std::optional<Wdc5Verification> VerifyWdc5(std::filesystem::path const& path, std::string& error)
{
    auto const header = InspectHeader(path, error);
    if (header.format != FileFormat::WDC5)
    {
        if (error.empty()) error = "Expected a WDC5 file.";
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    stream.seekg(0, std::ios::end);
    auto const length = stream.tellg();
    if (length < 204)
    {
        error = "WDC5 file is shorter than its 204-byte header.";
        return std::nullopt;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!stream)
    {
        error = "Could not read the complete WDC5 file.";
        return std::nullopt;
    }
    auto u16 = [&](std::size_t offset) -> std::uint16_t {
        return static_cast<std::uint16_t>(ReadUnsigned(bytes.data() + offset, 2));
    };
    auto u32 = [&](std::size_t offset) -> std::uint32_t {
        return static_cast<std::uint32_t>(ReadUnsigned(bytes.data() + offset, 4));
    };
    auto u64 = [&](std::size_t offset) -> std::uint64_t { return ReadUnsigned(bytes.data() + offset, 8); };
    auto fail = [&](std::string message) -> std::optional<Wdc5Verification> {
        error = std::move(message);
        return std::nullopt;
    };

    if (header.schemaVersion != 5)
        return fail("Unsupported WDC5 schema version " + std::to_string(header.schemaVersion) + ".");
    if (header.schemaTag.empty())
        return fail("WDC5 schema tag is empty.");
    if (header.fieldCount > 4096 || header.totalFieldCount > 4096 || header.sectionCount > 65536)
        return fail("WDC5 header contains unreasonable field or section counts.");
    if (header.totalFieldCount < header.fieldCount)
        return fail("WDC5 total field count is smaller than field count.");
    if (header.sectionCount == 0 || header.recordCount == 0)
    {
        Wdc5Verification empty;
        empty.header = header;
        error.clear();
        return empty;
    }

    auto const sectionHeadersEnd = std::uint64_t { 204 } + std::uint64_t { header.sectionCount } * 40u;
    auto const fieldMetaEnd = sectionHeadersEnd + std::uint64_t { header.fieldCount } * 4u;
    auto const columnMetaEnd = fieldMetaEnd + std::uint64_t { header.fieldCount } * 24u;
    if (columnMetaEnd > bytes.size())
        return fail("WDC5 field/column metadata extends past end of file.");

    std::uint64_t calculatedPallet = 0;
    std::uint64_t calculatedCommon = 0;
    for (std::uint32_t index = 0; index < header.fieldCount; ++index)
    {
        auto const offset = static_cast<std::size_t>(fieldMetaEnd + std::uint64_t { index } * 24u);
        auto const additionalSize = u32(offset + 4);
        auto const compression = u32(offset + 8);
        if (compression > 5)
            return fail("Column " + std::to_string(index) + " has unknown compression type " + std::to_string(compression) + ".");
        if (compression == 2) calculatedCommon += additionalSize;
        if (compression == 3 || compression == 4) calculatedPallet += additionalSize;
    }
    auto const columnMetadataSize = u32(188);
    auto const commonSize = u32(192);
    auto const palletSize = u32(196);
    if (columnMetadataSize != std::uint64_t { header.fieldCount } * 24u)
        return fail("Column-metadata size does not match field count.");
    if (calculatedCommon != commonSize)
        return fail("Common-data size does not match column metadata.");
    if (calculatedPallet != palletSize)
        return fail("Pallet-data size does not match column metadata.");
    auto const metadataEnd = columnMetaEnd + calculatedPallet + calculatedCommon;
    if (metadataEnd > bytes.size())
        return fail("WDC5 auxiliary column data extends past end of file.");

    Wdc5Verification result;
    result.header = header;
    std::uint64_t previousSectionEnd = metadataEnd;
    for (std::uint32_t index = 0; index < header.sectionCount; ++index)
    {
        auto const offset = std::size_t { 204 } + std::size_t { index } * 40u;
        auto const tactKey = u64(offset);
        auto const fileOffset = u32(offset + 8);
        auto const recordCount = u32(offset + 12);
        auto const stringSize = u32(offset + 16);
        auto const offsetRecordsEnd = u32(offset + 20);
        auto const indexDataSize = u32(offset + 24);
        auto const parentLookupSize = u32(offset + 28);
        auto const offsetMapCount = u32(offset + 32);
        auto const copyCount = u32(offset + 36);
        if (fileOffset < previousSectionEnd || fileOffset > bytes.size())
            return fail("Section " + std::to_string(index) + " has an invalid or overlapping file offset.");
        std::uint64_t recordsEnd = 0;
        if ((header.flags & 0x1u) != 0)
        {
            ++result.sparseSections;
            recordsEnd = offsetRecordsEnd;
            if (recordsEnd < fileOffset)
                return fail("Sparse section " + std::to_string(index) + " ends before it starts.");
        }
        else
        {
            recordsEnd = std::uint64_t { fileOffset } + std::uint64_t { recordCount } * header.recordSize + stringSize;
        }
        auto const sectionEnd = recordsEnd + indexDataSize + parentLookupSize +
            std::uint64_t { copyCount } * 8u + std::uint64_t { offsetMapCount } * 10u;
        if (sectionEnd > bytes.size())
            return fail("Section " + std::to_string(index) + " extends past end of file.");
        previousSectionEnd = sectionEnd;
        result.decodedSectionRecords += recordCount;
        result.copyRows += copyCount;
        if (tactKey != 0) ++result.encryptedSections;
    }
    if (header.sectionCount != 0 && result.decodedSectionRecords != header.recordCount)
        return fail("Header record count does not equal the sum of section record counts.");
    error.clear();
    return result;
}

std::optional<PtchVerification> VerifyPtch(std::filesystem::path const& path, std::string& error)
{
    constexpr std::size_t headerSize = 68;
    constexpr std::size_t transformOffset = 56;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        error = "Could not open " + path.string();
        return std::nullopt;
    }
    auto const length = stream.tellg();
    if (length < static_cast<std::streamoff>(headerSize))
    {
        error = "PTCH file is shorter than its 68-byte header.";
        return std::nullopt;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!stream)
    {
        error = "Failed while reading PTCH contents.";
        return std::nullopt;
    }
    auto u32 = [&](std::size_t offset) { return static_cast<std::uint32_t>(ReadUnsigned(bytes.data() + offset, 4)); };
    auto fail = [&](std::string message) -> std::optional<PtchVerification> {
        error = std::move(message);
        return std::nullopt;
    };
    if (std::memcmp(bytes.data(), "PTCH", 4) != 0 || std::memcmp(bytes.data() + 16, "MD5_", 4) != 0 ||
        std::memcmp(bytes.data() + transformOffset, "XFRM", 4) != 0)
        return fail("PTCH, MD5_, or XFRM signature is invalid.");
    if (u32(20) != 40)
        return fail("The MD5 block does not have the expected 40-byte size.");

    PtchVerification result;
    result.decompressedPatchSize = u32(4);
    result.sizeBefore = u32(8);
    result.sizeAfter = u32(12);
    result.transformBlockSize = u32(60);
    result.patchType.assign(reinterpret_cast<char const*>(bytes.data() + 64), 4);
    if (result.decompressedPatchSize < headerSize)
        return fail("The declared decompressed patch size is smaller than the header.");
    if (result.transformBlockSize < 12 || transformOffset + result.transformBlockSize != bytes.size())
        return fail("The XFRM block size does not match the physical file size.");
    if (result.patchType != "BSD0" && result.patchType != "COPY")
        return fail("Unsupported PTCH transform type '" + result.patchType + "'.");

    auto const decompressedSize = static_cast<std::size_t>(result.decompressedPatchSize) - headerSize;
    auto const storedSize = static_cast<std::size_t>(result.transformBlockSize) - 12;
    result.compressed = storedSize < decompressedSize;
    std::vector<unsigned char> payload(decompressedSize);
    auto const* stored = bytes.data() + headerSize;
    if (!result.compressed)
    {
        if (storedSize != decompressedSize)
            return fail("Uncompressed PTCH payload has an inconsistent size.");
        std::copy_n(stored, storedSize, payload.begin());
    }
    else
    {
        if (storedSize < 4 || ReadUnsigned(stored, 4) != decompressedSize)
            return fail("Compressed PTCH payload has an invalid decompressed-size prefix.");
        std::size_t input = 4, output = 0;
        while (input < storedSize && output < decompressedSize)
        {
            auto const marker = stored[input++];
            auto const count = static_cast<std::size_t>(marker & 0x7f) + 1;
            if ((marker & 0x80) != 0)
            {
                if (input + count > storedSize || output + count > decompressedSize)
                    return fail("Compressed PTCH literal run exceeds its declared bounds.");
                std::copy_n(stored + input, count, payload.begin() + static_cast<std::ptrdiff_t>(output));
                input += count;
            }
            else if (output + count > decompressedSize)
                return fail("Compressed PTCH zero run exceeds its declared bounds.");
            output += count;
        }
        if (output != decompressedSize)
            return fail("Compressed PTCH payload does not expand to its declared size.");
    }

    if (result.patchType == "BSD0")
    {
        if (payload.size() < 32 || std::memcmp(payload.data(), "BSDIFF40", 8) != 0)
            return fail("BSD0 payload does not contain a BSDIFF40 header.");
        auto const controlSize = ReadUnsigned(payload.data() + 8, 8);
        auto const dataSize = ReadUnsigned(payload.data() + 16, 8);
        auto const newSize = ReadUnsigned(payload.data() + 24, 8);
        if (newSize != result.sizeAfter)
            return fail("BSDIFF output size does not match the PTCH after-size.");
        if (controlSize > payload.size() - 32 || dataSize > payload.size() - 32 - controlSize)
            return fail("BSDIFF control or data block exceeds the decompressed payload.");
        if ((controlSize % 12) != 0)
            return fail("BSDIFF control block is not a sequence of 12-byte entries.");
        auto const dataStart = std::size_t { 32 } + static_cast<std::size_t>(controlSize);
        auto const extraStart = dataStart + static_cast<std::size_t>(dataSize);
        std::size_t control = 32, dataUsed = 0, extraUsed = 0;
        std::uint64_t produced = 0;
        while (produced < newSize)
        {
            if (control + 12 > dataStart)
                return fail("BSDIFF control block ends before the output is complete.");
            auto const addLength = static_cast<std::uint32_t>(ReadUnsigned(payload.data() + control, 4));
            auto const moveLength = static_cast<std::uint32_t>(ReadUnsigned(payload.data() + control + 4, 4));
            control += 12;
            if (produced + addLength + moveLength > newSize || dataUsed + addLength > dataSize ||
                extraUsed + moveLength > payload.size() - extraStart)
                return fail("BSDIFF control entry exceeds its declared blocks.");
            produced += static_cast<std::uint64_t>(addLength) + moveLength;
            dataUsed += addLength;
            extraUsed += moveLength;
        }
    }
    else if (decompressedSize != result.sizeAfter)
        return fail("COPY payload size does not match the PTCH after-size.");

    error.clear();
    return result;
}

std::optional<Table> BinaryTableCodec::LoadWdbc(std::filesystem::path const& path,
    std::vector<Column> columns, std::string& error)
{
    auto const header = InspectHeader(path, error);
    if (header.format != FileFormat::WDBC)
    {
        if (error.empty()) error = "This codec only reads WDBC files.";
        return std::nullopt;
    }
    auto const logicalFieldCount = static_cast<std::size_t>(std::count_if(columns.begin(), columns.end(),
        [](Column const& column) { return !column.padding; }));
    if (logicalFieldCount != header.fieldCount)
    {
        error = "Definition expands to " + std::to_string(logicalFieldCount) + " non-padding fields, but the file header declares " +
            std::to_string(header.fieldCount) + ". Check the selected build and definition.";
        return std::nullopt;
    }
    std::size_t expectedRecordSize = 0;
    for (auto const& column : columns) expectedRecordSize += StorageBytes(column);
    if (expectedRecordSize != header.recordSize)
    {
        error = "Definition describes " + std::to_string(expectedRecordSize) + " bytes per row, but the file header declares " +
            std::to_string(header.recordSize) + ". Check the selected build and definition.";
        return std::nullopt;
    }
    auto const expectedMinimum = std::uintmax_t { 20 } +
        static_cast<std::uintmax_t>(header.recordCount) * header.recordSize + header.stringBlockSize;
    std::error_code sizeError;
    auto const fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize < expectedMinimum)
    {
        error = "The WDBC data or string block is truncated.";
        return std::nullopt;
    }

    std::ifstream stream(path, std::ios::binary);
    stream.seekg(20);
    auto const dataSize = static_cast<std::size_t>(header.recordCount) * header.recordSize;
    std::vector<unsigned char> records(dataSize);
    stream.read(reinterpret_cast<char*>(records.data()), static_cast<std::streamsize>(records.size()));
    std::vector<char> strings(header.stringBlockSize);
    stream.read(strings.data(), static_cast<std::streamsize>(strings.size()));
    if (!stream)
    {
        error = "Failed while reading WDBC contents.";
        return std::nullopt;
    }

    Table table;
    table.path = path;
    table.format = FileFormat::WDBC;
    table.columns = std::move(columns);
    table.rows.reserve(header.recordCount);
    for (std::uint32_t rowIndex = 0; rowIndex < header.recordCount; ++rowIndex)
    {
        Row row;
        row.stableId = table.nextStableId++;
        row.cells.reserve(table.columns.size());
        std::size_t recordOffset = static_cast<std::size_t>(rowIndex) * header.recordSize;
        for (std::size_t columnIndex = 0; columnIndex < table.columns.size(); ++columnIndex)
        {
            auto const byteCount = StorageBytes(table.columns[columnIndex]);
            auto const bits = ReadUnsigned(records.data() + recordOffset, byteCount);
            recordOffset += byteCount;
            switch (table.columns[columnIndex].type)
            {
            case ValueType::String:
                if (bits >= strings.size())
                {
                    error = "Row " + std::to_string(rowIndex) + " contains an out-of-range string offset.";
                    return std::nullopt;
                }
                {
                    auto const offset = static_cast<std::size_t>(bits);
                    auto const* start = strings.data() + offset;
                    auto const remaining = strings.size() - offset;
                    auto const* end = static_cast<char const*>(std::memchr(start, '\0', remaining));
                    if (!end)
                    {
                        error = "The WDBC string block contains an unterminated string.";
                        return std::nullopt;
                    }
                    row.cells.emplace_back(std::string(start, end));
                }
                break;
            case ValueType::Float:
                row.cells.emplace_back(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits))));
                break;
            case ValueType::UnsignedInteger:
                row.cells.emplace_back(static_cast<std::uint64_t>(bits));
                break;
            case ValueType::SignedInteger:
                if (byteCount >= 8)
                    row.cells.emplace_back(static_cast<std::int64_t>(bits));
                else
                {
                    auto const shift = static_cast<unsigned>(64 - byteCount * 8);
                    row.cells.emplace_back(static_cast<std::int64_t>(bits << shift) >> shift);
                }
                break;
            }
        }
        table.rows.push_back(std::move(row));
    }
    error.clear();
    return table;
}

bool BinaryTableCodec::SaveWdbc(Table const& table, std::filesystem::path const& path, std::string& error)
{
    if (table.format != FileFormat::WDBC)
    {
        error = "Writing is not enabled for " + std::string(FormatName(table.format)) + " yet.";
        return false;
    }
    std::vector<char> strings { '\0' };
    std::unordered_map<std::string, std::uint32_t> offsets;
    offsets.emplace("", 0);
    std::vector<std::uint64_t> values;
    values.reserve(table.rows.size() * table.columns.size());
    for (auto const& row : table.rows)
    {
        if (row.cells.size() != table.columns.size())
        {
            error = "A row has the wrong number of cells.";
            return false;
        }
        for (std::size_t index = 0; index < row.cells.size(); ++index)
        {
            if (table.columns[index].type != ValueType::String)
            {
                auto const bits = table.columns[index].storageBits;
                if (table.columns[index].type == ValueType::UnsignedInteger)
                {
                    auto const item = std::get_if<std::uint64_t>(&row.cells[index]);
                    auto const maximum = bits >= 64 ? std::numeric_limits<std::uint64_t>::max() : ((std::uint64_t { 1 } << bits) - 1);
                    if (!item || *item > maximum)
                    {
                        error = "Value in column '" + table.columns[index].name + "' does not fit its unsigned " + std::to_string(bits) + "-bit field.";
                        return false;
                    }
                }
                else if (table.columns[index].type == ValueType::SignedInteger)
                {
                    auto const item = std::get_if<std::int64_t>(&row.cells[index]);
                    auto const minimum = bits >= 64 ? std::numeric_limits<std::int64_t>::min() : -(std::int64_t { 1 } << (bits - 1));
                    auto const maximum = bits >= 64 ? std::numeric_limits<std::int64_t>::max() : ((std::int64_t { 1 } << (bits - 1)) - 1);
                    if (!item || *item < minimum || *item > maximum)
                    {
                        error = "Value in column '" + table.columns[index].name + "' does not fit its signed " + std::to_string(bits) + "-bit field.";
                        return false;
                    }
                }
                values.push_back(NumericBits(row.cells[index]));
                continue;
            }
            auto const& value = std::get<std::string>(row.cells[index]);
            auto found = offsets.find(value);
            if (found == offsets.end())
            {
                if (strings.size() + value.size() + 1 > std::numeric_limits<std::uint32_t>::max())
                {
                    error = "The rebuilt string block is too large.";
                    return false;
                }
                auto const offset = static_cast<std::uint32_t>(strings.size());
                strings.insert(strings.end(), value.begin(), value.end());
                strings.push_back('\0');
                found = offsets.emplace(value, offset).first;
            }
            values.push_back(found->second);
        }
    }

    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        error = "Could not create temporary file beside the destination.";
        return false;
    }
    stream.write("WDBC", 4);
    WriteU32(stream, static_cast<std::uint32_t>(table.rows.size()));
    WriteU32(stream, static_cast<std::uint32_t>(std::count_if(table.columns.begin(), table.columns.end(),
        [](Column const& column) { return !column.padding; })));
    std::uint32_t recordSize = 0;
    for (auto const& column : table.columns) recordSize += static_cast<std::uint32_t>(StorageBytes(column));
    WriteU32(stream, recordSize);
    WriteU32(stream, static_cast<std::uint32_t>(strings.size()));
    std::size_t valueIndex = 0;
    for (auto const& row : table.rows)
        for (std::size_t columnIndex = 0; columnIndex < row.cells.size(); ++columnIndex)
            WriteUnsigned(stream, values[valueIndex++], StorageBytes(table.columns[columnIndex]));
    stream.write(strings.data(), static_cast<std::streamsize>(strings.size()));
    stream.close();
    if (!stream)
    {
        error = "Failed while writing the temporary file.";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    std::error_code filesystemError;
    if (std::filesystem::exists(path))
    {
        auto backup = path;
        backup += ".bak";
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, filesystemError);
        if (filesystemError)
        {
            error = "Could not create backup: " + filesystemError.message();
            std::filesystem::remove(temporary, filesystemError);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError)
    {
        std::filesystem::remove(path, filesystemError);
        filesystemError.clear();
        std::filesystem::rename(temporary, path, filesystemError);
    }
    if (filesystemError)
    {
        error = "Could not replace destination: " + filesystemError.message();
        return false;
    }
    error.clear();
    return true;
}
}
