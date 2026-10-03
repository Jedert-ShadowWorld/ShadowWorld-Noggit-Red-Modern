#pragma once

#include <core/Table.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <cmath>

namespace Noggit::ClientData
{
  inline double sampleNativeLightBand(std::map<std::uint32_t, double> const& keys,
      std::uint32_t time, bool packedColor)
  {
    if (keys.empty()) throw std::runtime_error("An edited LightData channel cannot have no keys.");
    auto upper = keys.upper_bound(time);
    auto lower = upper == keys.begin() ? std::prev(keys.end()) : std::prev(upper);
    if (lower->first == time || keys.size() == 1) return lower->second;
    auto next = upper == keys.end() ? keys.begin() : upper;
    double start = lower->first, end = next->first, at = time;
    if (end <= start) end += 2880;
    if (at < start) at += 2880;
    double factor = (at - start) / (end - start);
    if (!packedColor) return lower->second + (next->second - lower->second) * factor;
    std::uint32_t color = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
      auto a = (static_cast<std::uint32_t>(lower->second) >> shift) & 255;
      auto b = (static_cast<std::uint32_t>(next->second) >> shift) & 255;
      color |= static_cast<std::uint32_t>(std::lround(a + (static_cast<double>(b) - a) * factor)) << shift;
    }
    return color;
  }

  struct NativeRowChange
  {
    std::uint32_t id;
    bool added;
    std::map<std::size_t, dbc::CellValue> fields;
  };

  inline dbc::CellValue nativeDefault(dbc::Column const& column)
  {
    switch (column.type)
    {
      case dbc::ValueType::String: return std::string{};
      case dbc::ValueType::Float: return 0.0;
      case dbc::ValueType::UnsignedInteger: return std::uint64_t{0};
      default: return std::int64_t{0};
    }
  }

  // Apply the whole delta to a copy so a conflict cannot leave a partial edit.
  inline void applyNativeEditorChanges(dbc::Table& table, std::size_t idColumn,
      std::vector<NativeRowChange> const& changes, std::set<std::uint32_t> const& removed)
  {
    auto rows = table.rows;
    auto nextStableId = table.nextStableId;
    std::set<std::uint32_t> seen;
    for (auto const& change : changes)
    {
      if (!seen.insert(change.id).second || removed.contains(change.id))
        throw std::runtime_error("Conflicting editor row changes.");
      auto found = std::find_if(rows.begin(), rows.end(), [&](auto const& row) {
        return dbc::CellToString(row.cells.at(idColumn)) == std::to_string(change.id);
      });
      if (change.added != (found == rows.end()))
        throw std::runtime_error("Native/editor ID conflict: " + std::to_string(change.id));
      if (change.added)
      {
        dbc::Row row;
        row.stableId = nextStableId++;
        for (auto const& column : table.columns) row.cells.push_back(nativeDefault(column));
        std::string error;
        if (!dbc::SetCellFromString(row.cells.at(idColumn), table.columns.at(idColumn).type,
                                   std::to_string(change.id), error)) throw std::runtime_error(error);
        rows.push_back(std::move(row));
        found = std::prev(rows.end());
      }
      for (auto const& [column, value] : change.fields)
      {
        if (column == idColumn) throw std::runtime_error("A delta cannot rewrite the native ID.");
        found->cells.at(column) = value;
      }
    }
    std::erase_if(rows, [&](auto const& row) {
      return removed.contains(static_cast<std::uint32_t>(std::stoul(dbc::CellToString(row.cells.at(idColumn)))));
    });
    table.rows = std::move(rows);
    table.nextStableId = nextStableId;
  }
}
