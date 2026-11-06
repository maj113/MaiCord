# Build-time generated inputs of the Linux build (cmake/generated_sources.cmake is the Windows one, which also compiles
# shaders and the icon with the Windows SDK). Everything is written under ${MAICORD_GENERATED_DIRECTORY} in the
# build directory; a library that consumes an output lists it as OBJECT_DEPENDS and takes the directory as an include
# path. Only what the libraries built so far need.

set(maicord_generated_include_options "-I${CMAKE_CURRENT_BINARY_DIR}/generated;-Wno-c23-extensions")
include(cmake/packed_data.cmake)
maicord_embed_pack(src/ui/text/emoji_catalog.cpp "${MAICORD_EMOJI_CATALOG_INCLUDE}" "${MAICORD_EMOJI_POOL_INCLUDE}")
maicord_embed_pack(src/ui/widgets/icon_pack.cpp "${MAICORD_ICON_PACK_INCLUDE}")
maicord_embed_pack(src/app/media/regional_flag_assets.cpp "${MAICORD_REGIONAL_FLAG_BUNDLE}")

# The #embed sits in an include of its own because a quoted #embed is looked up beside the file that names it.
file(WRITE "${MAICORD_GENERATED_DIRECTORY}/app_icon.inc" "#embed \"maicord.ico\"\n")
