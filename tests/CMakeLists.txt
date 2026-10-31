cmake_minimum_required(VERSION 3.16)
project(NoggitRegressionTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

set(NOGGIT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../src")
function(noggit_test name)
  cmake_parse_arguments(TEST "" "" "ARGS" ${ARGN})
  add_executable(${name} ${TEST_UNPARSED_ARGUMENTS})
  target_include_directories(${name} PRIVATE "${NOGGIT_SOURCE}" "${NOGGIT_SOURCE}/external")
  add_test(NAME ${name} COMMAND ${name} ${TEST_ARGS})
endfunction()

foreach(name IN ITEMS
    load_failures uid_repair retail_liquid_lod retail_asset_loading blp_bgra
    retail_skin wmo_group_ids map_preview_quality retail_m2_animation
    retail_world_tables retail_map_texture retail_root_layout
    retail_object_tables retail_object_transfer)
  noggit_test(${name} ${name}.cpp)
endforeach()
noggit_test(uid_geometry uid_geometry.cpp "${NOGGIT_SOURCE}/noggit/formats/adt/UidRepairGeometry.cpp")
noggit_test(terrain_lod_updater terrain_lod_updater.cpp
  "${NOGGIT_SOURCE}/noggit/formats/adt/TerrainLodUpdater.cpp")

find_package(Qt5 QUIET COMPONENTS Core Widgets)
if(Qt5_FOUND)
  function(noggit_gl_test name)
    noggit_test(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE Qt5::Widgets)
    if(WIN32)
      target_link_libraries(${name} PRIVATE opengl32)
    endif()
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
  endfunction()
  noggit_gl_test(m2_alpha_key ARGS "${NOGGIT_SOURCE}/noggit/rendering/glsl")
  noggit_gl_test(wmo_retail_material
    "${NOGGIT_SOURCE}/opengl/context.cpp" "${NOGGIT_SOURCE}/noggit/Log.cpp"
    ARGS "${NOGGIT_SOURCE}/noggit/rendering/glsl/wmo_frag.glsl")
  noggit_gl_test(shader_vao_state
    "${NOGGIT_SOURCE}/opengl/shader.cpp" "${NOGGIT_SOURCE}/opengl/context.cpp"
    "${NOGGIT_SOURCE}/opengl/texture.cpp" "${NOGGIT_SOURCE}/noggit/Log.cpp"
    "${NOGGIT_SOURCE}/noggit/Misc.cpp" "${NOGGIT_SOURCE}/math/bounding_box.cpp")
  noggit_gl_test(texture_upload_binding)
  noggit_gl_test(model_buffer_state
    "${NOGGIT_SOURCE}/opengl/context.cpp" "${NOGGIT_SOURCE}/noggit/Log.cpp")
  noggit_test(log_archive log_archive.cpp "${NOGGIT_SOURCE}/noggit/Log.cpp")
  target_link_libraries(log_archive PRIVATE Qt5::Core)
  noggit_test(uid_repair_review uid_repair_review.cpp)
  target_link_libraries(uid_repair_review PRIVATE Qt5::Widgets)
  set_tests_properties(uid_repair_review PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  noggit_test(derived_map_state derived_map_state.cpp
    "${NOGGIT_SOURCE}/noggit/DerivedMapState.cpp" "${NOGGIT_SOURCE}/noggit/TileIndex.cpp")
  target_link_libraries(derived_map_state PRIVATE Qt5::Core)
else()
  message(STATUS "Qt5 Core/Widgets SDK not found: Qt and OpenGL regressions are unavailable.")
endif()

# These fixtures execute selected production methods with lightweight test doubles.
# Keep their generated sources in the build directory, never in the checkout.
if(WIN32)
  function(noggit_method_test name fixture)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${fixture}-regression.cpp")
    set(script "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare-${fixture}-regression.ps1")
    add_custom_command(OUTPUT "${output}"
      COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${script}" -OutputPath "${output}"
      DEPENDS "${script}" "${CMAKE_CURRENT_LIST_DIR}/${fixture}-regression.inc" ${ARGN}
      VERBATIM)
    noggit_test(${name} "${output}")
  endfunction()
  noggit_method_test(m2_texture_lifecycle m2-lifecycle
    "${NOGGIT_SOURCE}/noggit/rendering/ModelRender.cpp" "${NOGGIT_SOURCE}/noggit/Model.cpp")
  noggit_method_test(wmo_render_lifecycle wmo-render
    "${NOGGIT_SOURCE}/noggit/rendering/WMOGroupRender.cpp"
    "${NOGGIT_SOURCE}/noggit/rendering/WMOGroupRender.hpp"
    "${NOGGIT_SOURCE}/noggit/TextureManager.cpp")
  noggit_method_test(object_clipboard object-clipboard "${NOGGIT_SOURCE}/noggit/ui/ObjectEditor.cpp")
  if(Qt5_FOUND)
    noggit_method_test(terrain_export_lifecycle terrain-export
      "${NOGGIT_SOURCE}/noggit/tools/MinimapTool.cpp" "${NOGGIT_SOURCE}/noggit/MapView.cpp")
    target_link_libraries(terrain_export_lifecycle PRIVATE Qt5::Widgets)
    set_tests_properties(terrain_export_lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    noggit_method_test(terrain_lod_reference lod-reference
      "${NOGGIT_SOURCE}/noggit/formats/adt/ShadowlandsADTCodec.cpp"
      "${NOGGIT_SOURCE}/noggit/formats/adt/ShadowlandsADTCodec.hpp"
      "${CMAKE_CURRENT_LIST_DIR}/terrain_lod_updater.cpp")
    target_sources(terrain_lod_reference PRIVATE "${NOGGIT_SOURCE}/noggit/formats/adt/TerrainLodUpdater.cpp")
    target_include_directories(terrain_lod_reference PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(terrain_lod_reference PRIVATE Qt5::Core)
  endif()
  # The existing OBJ1 fixture uses a separate main include.
  set(output "${CMAKE_CURRENT_BINARY_DIR}/obj1-regression.cpp")
  add_custom_command(OUTPUT "${output}"
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare-obj1-regression.ps1" -OutputPath "${output}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare-obj1-regression.ps1"
      "${NOGGIT_SOURCE}/noggit/formats/adt/ShadowlandsADTCodec.cpp"
      "${CMAKE_CURRENT_LIST_DIR}/obj1-regression-main.inc"
    VERBATIM)
  noggit_test(retail_obj1_writer "${output}")
endif()
