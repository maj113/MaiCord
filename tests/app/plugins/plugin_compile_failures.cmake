# Compiles tests/app/plugins/plugin_compile_failures.cpp with MC_CASE_<CASE>
# defined and the flags plugins build with (cmake/test_support.cmake,
# maicord_plugin): with an empty EXPECT it must compile cleanly, otherwise it
# must fail with EXPECT in the diagnostics.
# cmake -DCOMPILER=<clang-cl> -DSOURCE_DIR=<repository> -DCASE=<case> -DEXPECT=<text> -P plugin_compile_failures.cmake
execute_process(
    COMMAND "${COMPILER}" /nologo /Zs /std:c++latest /W4 /WX /utf-8 /GR- /clang:-nostdinc
        "-I${SOURCE_DIR}/include" "-I${SOURCE_DIR}/include/maicord/sys/libc" /DMC_RUNTIME "/DMC_CASE_${CASE}"
        "${SOURCE_DIR}/tests/app/plugins/plugin_compile_failures.cpp"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(EXPECT STREQUAL "")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "MC_CASE_${CASE} failed to compile:\n${output}")
    endif()
elseif(result EQUAL 0)
    message(FATAL_ERROR "MC_CASE_${CASE} compiled; expected: ${EXPECT}")
else()
    string(FIND "${output}" "${EXPECT}" at)
    if(at LESS 0)
        message(FATAL_ERROR "MC_CASE_${CASE} failed without \"${EXPECT}\":\n${output}")
    endif()
endif()
