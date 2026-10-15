# Idempotent `git apply` for FetchContent/ExternalProject PATCH_COMMAND. Run from the fetched source
# directory (the PATCH_COMMAND working directory):
#   cmake -DGIT_EXECUTABLE=<git> -DPATCH_FILE=<file.patch> -P applyPatchOnce.cmake
# A reconfigure re-runs PATCH_COMMAND on an already-patched tree; when the patch reverse-applies
# cleanly it is already in place and nothing is changed.

if(NOT GIT_EXECUTABLE OR NOT PATCH_FILE)
	message(FATAL_ERROR "applyPatchOnce.cmake needs GIT_EXECUTABLE and PATCH_FILE")
endif()

execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${PATCH_FILE}"
	RESULT_VARIABLE reverse_result
	OUTPUT_QUIET ERROR_QUIET)
if(reverse_result EQUAL 0)
	return()
endif()

execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH_FILE}"
	RESULT_VARIABLE apply_result)
if(NOT apply_result EQUAL 0)
	message(FATAL_ERROR "failed to apply ${PATCH_FILE}")
endif()
