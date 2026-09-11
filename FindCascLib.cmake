# This file is part of Noggit3, licensed under GNU General Public License (version 3).

# adds target CascLib
FetchContent_Declare(
  casclib
  GIT_REPOSITORY https://gitlab.com/prophecy-rp/dependencies.git
  GIT_TAG        dep-casclib
)

FetchContent_GetProperties(casclib)
if(NOT casclib)
  MESSAGE(STATUS "---------------------------------------------")
  MESSAGE(STATUS "Installing Casclib...")
  FetchContent_Populate(casclib)
  SET(CASCLIB_INCLUDE_DIR "${casclib_SOURCE_DIR}/includes")

  if (UNIX)
    if (NOT APPLE)
      SET(CASCLIB_LIBRARY_DEBUG_DIR "${casclib_SOURCE_DIR}/lib/debug/x64-linux")
      SET(CASCLIB_LIBRARY_RELEASE_DIR "${casclib_SOURCE_DIR}/lib/release/x64-linux")
    else()
      # handle Mac here
      if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
        SET(CASCLIB_LIBRARY_DEBUG_DIR "${casclib_SOURCE_DIR}/lib/debug/x64-amd64-mac")
        SET(CASCLIB_LIBRARY_RELEASE_DIR "${casclib_SOURCE_DIR}/lib/release/x64-amd64-mac")
      else()
        message("No CASCLib available through FetchContent for Apple Silicon")
      endif()
    endif()
  else()
    SET(CASCLIB_LIBRARY_DEBUG_DIR "${casclib_SOURCE_DIR}/lib/debug/x64")
    SET(CASCLIB_LIBRARY_RELEASE_DIR "${casclib_SOURCE_DIR}/lib/release/x64")
  endif()
endif()

find_path (CASCLIB_INCLUDE_DIR CascLib.h CascPort.h PATHS "${casclib_SOURCE_DIR}/includes")

if (WIN32 AND EXISTS "${casclib_SOURCE_DIR}/includes/CascOpenStorage.cpp")
  set(CASCLIB_SOURCE_ROOT "${casclib_SOURCE_DIR}/includes")
  add_library (CascLib STATIC
    "${CASCLIB_SOURCE_ROOT}/CascDecompress.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascDecrypt.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascFiles.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascFindFile.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascIndexFiles.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascOpenFile.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascOpenStorage.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascReadFile.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascRootFile_Diablo3.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascRootFile_Install.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascRootFile_MNDX.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascRootFile_OW.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascRootFile_Text.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascRootFile_TVFS.cpp"
    "${CASCLIB_SOURCE_ROOT}/CascRootFile_WoW.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/Common.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/Csv.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/Directory.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/FileStream.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/FileTree.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/ListFile.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/Mime.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/RootHandler.cpp"
    "${CASCLIB_SOURCE_ROOT}/common/Sockets.cpp"
    "${CASCLIB_SOURCE_ROOT}/jenkins/lookup3.c"
    "${CASCLIB_SOURCE_ROOT}/md5/md5.cpp"
    "${CASCLIB_SOURCE_ROOT}/zlib/adler32.c"
    "${CASCLIB_SOURCE_ROOT}/zlib/crc32.c"
    "${CASCLIB_SOURCE_ROOT}/zlib/deflate.c"
    "${CASCLIB_SOURCE_ROOT}/zlib/inffast.c"
    "${CASCLIB_SOURCE_ROOT}/zlib/inflate.c"
    "${CASCLIB_SOURCE_ROOT}/zlib/inftrees.c"
    "${CASCLIB_SOURCE_ROOT}/zlib/trees.c"
    "${CASCLIB_SOURCE_ROOT}/zlib/zutil.c"
  )
  target_include_directories (CascLib SYSTEM PUBLIC "${CASCLIB_SOURCE_ROOT}")
  target_link_libraries (CascLib PUBLIC ws2_32)
  target_compile_definitions (CascLib PUBLIC -DCASCLIB_NO_AUTO_LINK_LIBRARY)
else()
  find_library (_casc_debug_lib NAMES CascLibDAD CascLibDAS CascLibDUD CascLibDUS casc casclib CascLib PATHS ${CASCLIB_LIBRARY_DEBUG_DIR})
  find_library (_casc_release_lib NAMES CascLibRAD CascLibRAS CascLibRUD CascLibRUS casc casclib CascLib PATHS ${CASCLIB_LIBRARY_RELEASE_DIR})
  find_library (_casc_any_lib NAMES casc casclib CascLib)

  set (CASC_LIBRARIES)
  if (_casc_debug_lib AND _casc_release_lib)
    list (APPEND CASC_LIBRARIES debug ${_casc_debug_lib} optimized ${_casc_release_lib})
  else()
    list (APPEND CASC_LIBRARIES ${_casc_any_lib})
  endif()

  add_library (CascLib INTERFACE)
  target_link_libraries (CascLib INTERFACE ${CASC_LIBRARIES})
  set_property  (TARGET CascLib APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${CASCLIB_INCLUDE_DIR})
  set_property  (TARGET CascLib APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${CASCLIB_INCLUDE_DIR})
  target_compile_definitions (CascLib INTERFACE -DCASCLIB_NO_AUTO_LINK_LIBRARY)
endif()

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (CascLib DEFAULT_MSG CASCLIB_INCLUDE_DIR)

mark_as_advanced (CASCLIB_INCLUDE_DIR _casc_debug_lib _casc_release_lib _casc_any_lib CASC_LIBRARIES)

MESSAGE(STATUS "Casclib Include         : ${CASCLIB_INCLUDE_DIR}")
MESSAGE(STATUS "Casclib Debug Lib       : ${_casc_debug_lib}")
MESSAGE(STATUS "Casclib Optimized Lib   : ${_casc_release_lib}")
MESSAGE(STATUS "Casclib Installed!")
MESSAGE(STATUS "---------------------------------------------")
