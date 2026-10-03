#include "core/Table.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace dbc
{
std::string_view FormatName(FileFormat format)
{
    switch (format)
    {
    case FileFormat::WDBC: return "WDBC";
    case FileFormat::WDB2: return "WDB2";
    case FileFormat::WDB3: return "WDB3";
    case FileFormat::WDB4: return "WDB4";
    case FileFormat::WDB5: return "WDB5";
    case FileFormat::WDB6: return "WDB6";
    case FileFormat::WDC1: return "WDC1";
    case FileFormat::WDC2: return "WDC2";
    case FileFormat::WDC3: return "WDC3";
    case FileFormat::WDC4: return "WDC4";
    case FileFormat::WDC5: return "WDC5";
    case FileFormat::WCH7: return "WCH7 hotfix table";
    case FileFormat::WCH8: return "WCH8 hotfix table";
    case FileFormat::XFTH: return "XFTH hotfix cache";
    case FileFormat::PTCH: return "PTCH (MPQ patch)";
    default: return "Unknown";
    }
}

std::string CellToString(CellValue const& value)
{
    return std::visit([](auto const& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>)
            return item;
        else if constexpr (std::is_same_v<T, double>)
        {
            std::ostringstream stream;
            stream << std::setprecision(9) << item;
            return stream.str();
        }
        else
            return std::to_string(item);
    }, value);
}

bool SetCellFromString(CellValue& value, ValueType type, std::string const& text, std::string& error)
{
    error.clear();
    if (type == ValueType::String)
    {
        value = text;
        return true;
    }
    if (type == ValueType::Float)
    {
        char* end = nullptr;
        auto const parsed = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || !std::isfinite(parsed))
        {
            error = "Expected a finite decimal number.";
            return false;
        }
        value = parsed;
        return true;
    }
    if (type == ValueType::SignedInteger)
    {
        std::int64_t parsed = 0;
        auto const result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
        if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
        {
            error = "Expected a signed integer.";
            return false;
        }
        value = parsed;
        return true;
    }
    std::uint64_t parsed = 0;
    auto const result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
    {
        error = "Expected an unsigned integer.";
        return false;
    }
    value = parsed;
    return true;
}
}
