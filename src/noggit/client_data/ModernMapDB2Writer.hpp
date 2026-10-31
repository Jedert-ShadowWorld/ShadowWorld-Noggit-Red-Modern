#pragma once

#include <cstdint>

class QWidget;
class DBCFile;
#include <string>
#include <vector>
#include <QStringList>

namespace Noggit::ClientData
{
  // Rows of a supplemental table: native DB2 for Retail projects, the CSV otherwise.
  std::vector<QStringList> supplementalRows(std::string const& table,
      std::string const& legacyCsv, std::vector<std::string> const& columns);
  // The wizard updates gMapDB first; this writes the corresponding native row.
  // False means the user cancelled. Failures throw without replacing the DB2.
  bool saveModernMapDB2(std::uint32_t map_id, QWidget* parent);
  void removeModernMapDB2(std::uint32_t map_id);
  void saveModernEditorDB2(DBCFile& legacy, std::string const& filename);
  void loadModernEditorSoundDB2();
  bool loadModernWMOAreaDB2();
  void loadRetailLiquidDB2();
  bool saveEditorDatabase(DBCFile& table, QWidget* parent);
}
