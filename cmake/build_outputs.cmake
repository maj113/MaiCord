# Transient ownership information for runtime-only finalization. This contains
# build products, never runtime/user directories. The wrapper removes it last.
function(maicord_collect_build_outputs directory)
    get_property(binary_directory DIRECTORY "${directory}" PROPERTY BINARY_DIR)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(kind ${target} TYPE)
        get_target_property(binary ${target} BINARY_DIR)
        get_target_property(source_directory ${target} SOURCE_DIR)
        if(kind MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            set_property(GLOBAL APPEND PROPERTY MAICORD_OWNED_OUTPUTS
                "objects|${binary}/CMakeFiles/${target}.dir\n")
            if(NOT kind STREQUAL "OBJECT_LIBRARY")
                set_property(GLOBAL APPEND PROPERTY MAICORD_OWNED_OUTPUTS "file|$<TARGET_FILE:${target}>\n")
                if(kind STREQUAL "SHARED_LIBRARY")
                    set_property(GLOBAL APPEND PROPERTY MAICORD_OWNED_OUTPUTS "file|$<TARGET_LINKER_FILE:${target}>\n")
                endif()
            endif()
        endif()
        # Utility targets can own generated inputs too; their DEPENDS/SOURCES
        # must not be excluded by the compiled-target type filter above.
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS sources)
            if(source MATCHES "\\$<")
                continue()
            endif()
            get_source_file_property(generated "${source}" DIRECTORY "${source_directory}" GENERATED)
            if(generated)
                cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${binary}" NORMALIZE OUTPUT_VARIABLE output)
                cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${output}" NORMALIZE contained)
                if(contained)
                    set_property(GLOBAL APPEND PROPERTY MAICORD_OWNED_OUTPUTS "file|${output}\n")
                endif()
            endif()
        endforeach()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY MAICORD_OWNED_OUTPUTS "file|${binary_directory}/cmake_install.cmake\n")
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        maicord_collect_build_outputs("${child}")
    endforeach()
endfunction()
maicord_collect_build_outputs("${CMAKE_BINARY_DIR}")
# These custom-command outputs are consumed through includes, resource
# dependencies or INTERFACE links rather than compiled-target SOURCES.
# Keep the finite ownership list tied to their defining CMake variables.
foreach(output IN ITEMS "${MAICORD_APP_ICON}" "${MAICORD_EMOJI_CATALOG_INCLUDE}"
        "${MAICORD_EMOJI_POOL_INCLUDE}" "${MAICORD_EMOJI_POOL_PACKED}"
        "${MAICORD_ICON_PACK_INCLUDE}" "${MAICORD_ICON_PACK_DATA}" "${MAICORD_ICON_PACK_PACKED}"
        "${MAICORD_REGIONAL_FLAG_BUNDLE}" "${MAICORD_REGIONAL_FLAG_PACKED}"
        "${MAICORD_SHADER_PACK_INCLUDE}" "${MAICORD_SHADER_PACK_PACKED}"
        "${maicord_combase_import_library}" "${maicord_sspicli_import_library}" ${MAICORD_PACKED_SHADER_HEADERS})
    cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${output}" NORMALIZE contained)
    if(NOT contained)
        message(FATAL_ERROR "Generated build output is outside the Release directory: ${output}")
    endif()
    set_property(GLOBAL APPEND PROPERTY MAICORD_OWNED_OUTPUTS "file|${output}\n")
endforeach()
get_property(maicord_owned_outputs GLOBAL PROPERTY MAICORD_OWNED_OUTPUTS)
string(JOIN "" maicord_owned_outputs ${maicord_owned_outputs})
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/maicord-build-outputs.txt"
    CONTENT "repository|${CMAKE_SOURCE_DIR}\n${maicord_owned_outputs}")
