include(FetchContent)

# Current retail clients use the TVFS/VFS root format supported by upstream CascLib.
FetchContent_Declare(
  casclib
  GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
  GIT_TAG        2a280f5a231966dc5d1b534978dd9f9f04a374cd
)

set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(CASC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(casclib)

add_library(CascLib ALIAS casc_static)
set(CASCLIB_INCLUDE_DIR "${casclib_SOURCE_DIR}/src")
set(CascLib_FOUND TRUE)
