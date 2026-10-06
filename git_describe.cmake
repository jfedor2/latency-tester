# Writes a header defining GIT_DESCRIBE: the exact tag name, or else the short
# commit hash, plus "-dirty" if applicable. Runs on every build; the header is
# only rewritten when it changes, to avoid needless recompiles.
#
# Usage: cmake -DSOURCE_DIR=<repo> -DOUTPUT=<header> -P git_describe.cmake

execute_process(
    COMMAND git describe --exact-match --dirty
    WORKING_DIRECTORY ${SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_DESCRIBE_RESULT
)
if(NOT GIT_DESCRIBE_RESULT EQUAL 0)
    # Not on a tag: plain hash rather than the "tag-N-gHASH" form.
    execute_process(
        COMMAND git describe --always --dirty --exclude=*
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT GIT_DESCRIBE)
    set(GIT_DESCRIBE "unknown")
endif()

set(CONTENT "#define GIT_DESCRIBE \"${GIT_DESCRIBE}\"\n")
if(EXISTS ${OUTPUT})
    file(READ ${OUTPUT} OLD_CONTENT)
    if(OLD_CONTENT STREQUAL CONTENT)
        return()
    endif()
endif()
file(WRITE ${OUTPUT} "${CONTENT}")
