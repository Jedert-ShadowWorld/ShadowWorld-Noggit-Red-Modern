add_library(barebones-dbc-core STATIC
  core/Table.cpp
  core/GameVersion.cpp
  schema/DbdParser.cpp
  formats/BinaryTable.cpp
  formats/LegacyDb2.cpp
  formats/Wdc5.cpp
)
target_include_directories(barebones-dbc-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(barebones-dbc-core PROPERTIES CXX_STANDARD 20 AUTOMOC OFF)
