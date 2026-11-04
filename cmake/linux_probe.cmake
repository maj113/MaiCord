# Throwaway discovery target: every unit of src/app (not shell) and src/ui, to list what does not compile on Linux yet.
file(GLOB_RECURSE MAICORD_PROBE_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/app/*.cpp" "${CMAKE_SOURCE_DIR}/src/ui/*.cpp")
list(FILTER MAICORD_PROBE_SOURCES EXCLUDE REGEX "^src/app/shell/|^src/ui/text/secure_text_store.cpp")
add_library(maicord_probe OBJECT EXCLUDE_FROM_ALL ${MAICORD_PROBE_SOURCES})
target_link_libraries(maicord_probe PRIVATE maicord_compile_contract)
target_include_directories(maicord_probe PRIVATE src/app "${MAICORD_GENERATED_DIRECTORY}")
target_compile_options(maicord_probe PRIVATE -include "${CMAKE_SOURCE_DIR}/cmake/probe_win32_compat.hpp")
set_target_properties(maicord_probe PROPERTIES CXX_EXTENSIONS OFF)
