#pragma once

#include <cstdint>

class QWidget;
class DBCFile;
#include <string>

namespace Noggit::ClientData
{
  // The wizard updates gMapDB first; this writes the corresponding native row.
  // False means the user cancelled. Failures throw without replacing the DB2.
  bool saveModernMapDB2(std::uint32_t map_id, QWidget* parent);
  void removeModernMapDB2(std::uint32_t map_id);
  void saveModernEditorDB2(DBCFile& legacy, std::string const& filename);
  void loadModernEditorSoundDB2();
  bool saveEditorDatabase(DBCFile& table, QWidget* parent);
}
