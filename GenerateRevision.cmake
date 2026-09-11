# This file is part of Noggit3, licensed under GNU General Public License (version 3).

# assumes working directory in git repo
# assumes var _noggit_revision_template_file
# assumes var _noggit_revision_output_file
# assumes var _noggit_revision_state_file
# assumes var GIT_EXECUTABLE
# generates file "${_noggit_revision_state_file}"
# generates file "${_noggit_revision_output_file}"


macro (execute _output_var) # , command...
  execute_process (COMMAND ${ARGN}
    OUTPUT_VARIABLE ${_output_var} OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _result)
  if (NOT _result EQUAL 0)
    message (FATAL_ERROR "'${ARGN}' failed.")
  endif()
endmacro()

set (_git_safe_directory "-c" "safe.directory=${CMAKE_SOURCE_DIR}")

execute (_status "${GIT_EXECUTABLE}" ${_git_safe_directory} "status" "--porcelain")
set (_dirty_marker "+")
if (_status STREQUAL "")
  set (_dirty_marker "")
endif()

# \todo If head is tagged, use tag.
# \todo Add deploy script that adds tags so that we have sane
# revisions again, at all times.
execute (_revision "${GIT_EXECUTABLE}" ${_git_safe_directory} "rev-parse" "--short" "HEAD")
execute (_commit_count "${GIT_EXECUTABLE}" ${_git_safe_directory} "rev-list" "--count" "--all")

file (READ "${_noggit_revision_template_file}" _template_blob)
file (READ "${CMAKE_CURRENT_LIST_FILE}" _self_blob)
string (SHA256 _state_hash "${_dirty_marker}${_revision}${_template_blob}${_self_blob}")

if (EXISTS ${_noggit_revision_state_file})
  file (READ "${_noggit_revision_state_file}" _old_state_hash)
  if (_state_hash STREQUAL _old_state_hash)
    return()
  endif()
endif()

file (WRITE "${_noggit_revision_state_file}" "${_state_hash}")

message("Commit count: ${_commit_count}")

set (NOGGIT_COMMIT_COUNT "${_commit_count}${_dirty_marker}")
set (NOGGIT_GIT_VERSION_STRING "${_revision}")
configure_file ("${_noggit_revision_template_file}"
                "${_noggit_revision_output_file}" @ONLY)
