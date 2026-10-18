# Applies one Kyty patch (Android port, address folding) to a FEX checkout. Run by fex.cmake as the ExternalProject PATCH_COMMAND:
#   cmake -DFEX_SOURCE_DIR=<checkout> -DFEX_PATCH=<fex-android.patch> -P applyFexPatch.cmake
# Idempotent: when the patch reverse-applies cleanly it is already in the tree and nothing is changed.

if(NOT FEX_SOURCE_DIR OR NOT FEX_PATCH)
	message(FATAL_ERROR "applyFexPatch.cmake needs FEX_SOURCE_DIR and FEX_PATCH")
endif()

execute_process(
	COMMAND git apply --reverse --check "${FEX_PATCH}"
	WORKING_DIRECTORY "${FEX_SOURCE_DIR}"
	RESULT_VARIABLE reverse_result
	OUTPUT_QUIET ERROR_QUIET)
if(reverse_result EQUAL 0)
	message(STATUS "FEX patch already applied: ${FEX_PATCH}")
	return()
endif()

execute_process(
	COMMAND git apply "${FEX_PATCH}"
	WORKING_DIRECTORY "${FEX_SOURCE_DIR}"
	RESULT_VARIABLE apply_result)
if(NOT apply_result EQUAL 0)
	message(FATAL_ERROR "failed to apply ${FEX_PATCH} to ${FEX_SOURCE_DIR}")
endif()
message(STATUS "Applied FEX patch: ${FEX_PATCH}")
