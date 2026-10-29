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
    load_failures retail_asset_loading blp_bgra
    retail_skin wmo_group_ids map_preview_quality retail_m2_animation
    retail_object_tables retail_object_transfer)
  noggit_test(${name} ${name}.cpp)
endforeach()

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
endif()
