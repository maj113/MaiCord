# Build-time generated inputs of the Linux build (cmake/generated_sources.cmake is the Windows one, which also compiles
# shaders and the icon with the Windows SDK). Everything is written under ${MAICORD_GENERATED_DIRECTORY} in the
# build directory; a library that consumes an output lists it as OBJECT_DEPENDS and takes the directory as an include
# path. Only what the libraries built so far need.

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
set(MAICORD_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MAICORD_GENERATED_DIRECTORY}")

# The permission grid (src/model/guild/permission_grid_data.inc) and the built-in games (src/app/account/game_data.inc)
# ship as zlib streams their owners inflate on first use, the same as the Windows build.
set(MAICORD_PERMISSION_GRID_PACK "${MAICORD_GENERATED_DIRECTORY}/permission_grid_pack.inc")
set(MAICORD_GAME_PACK "${MAICORD_GENERATED_DIRECTORY}/game_pack.inc")
add_custom_command(
    OUTPUT "${MAICORD_PERMISSION_GRID_PACK}" "${MAICORD_GAME_PACK}"
    BYPRODUCTS "${MAICORD_GENERATED_DIRECTORY}/permission_grid_packed.bin" "${MAICORD_GENERATED_DIRECTORY}/game_packed.bin"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_tables.py" "${MAICORD_GENERATED_DIRECTORY}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/model/guild/permission_grid_data.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/model/guild/permission_grid.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/model/guild/permission_bits.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/account/game_data.inc"
    DEPENDS tools/pack_tables.py tools/pack_settings_catalog.py tools/optimal_deflate.py
        tools/font_reference/embed_fixture.py
        src/model/guild/permission_grid_data.inc include/maicord/model/guild/permission_grid.hpp
        include/maicord/model/guild/permission_bits.hpp src/app/account/game_data.inc
    VERBATIM)
set_source_files_properties(src/model/guild/permission_grid.cpp PROPERTIES
    OBJECT_DEPENDS "${MAICORD_PERMISSION_GRID_PACK}"
    COMPILE_OPTIONS "-I${MAICORD_GENERATED_DIRECTORY};-Wno-c23-extensions")
