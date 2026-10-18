# Compiler and linker contract shared by every MaiCord image: freestanding
# C++23 (no CRT, exceptions or RTTI), Release-only code generation and one
# output directory, the build directory itself.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Every Win32 import is declared by the target that owns it. Suppress CMake's
# generic desktop-library bundle (GDI, spooler, OLE, COM dialogs, and others),
# which MaiCord otherwise asks LINK to scan despite /NODEFAULTLIB and no use.
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_C_STANDARD_LIBRARIES "")
option(MAICORD_GENERATE_LINK_MAPS
       "Emit symbol maps for binary-size and duplicate-code audits" OFF)

if(NOT WIN32)
    message(FATAL_ERROR "MaiCord targets Windows 10 or newer.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "The bootstrap build requires the Microsoft ABI (MSVC or clang-cl).")
endif()

# CMake injects exception handling by default for MSVC C++. MaiCord owns that
# decision per target and has no exception runtime.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Release must not produce compiler or linker PDBs, even when a stale cache
# added /Zi or /DEBUG. Optimization levels are each target's own (the
# contract's /O2 /Ob1), not CMake's default /O2 /Ob2 stated before them.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "")
foreach(flag_variable CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE CMAKE_ASM_MASM_FLAGS_RELEASE)
    set(clean_flags "${${flag_variable}}")
    foreach(pass 1 2)
        string(REGEX REPLACE "(^|[ \t])[-/](Z[iI7]|O2|Ob[0-3])([ \t]|$)" " " clean_flags "${clean_flags}")
    endforeach()
    string(STRIP "${clean_flags}" clean_flags)
    set(${flag_variable} "${clean_flags}" CACHE STRING "Release compiler flags" FORCE)
endforeach()
foreach(flag_variable CMAKE_EXE_LINKER_FLAGS_RELEASE CMAKE_SHARED_LINKER_FLAGS_RELEASE
                      CMAKE_MODULE_LINKER_FLAGS_RELEASE)
    string(REGEX REPLACE "(^|[ \t])[-/][Dd][Ee][Bb][Uu][Gg](:[A-Za-z]+)?([ \t]|$)" " "
           clean_flags "${${flag_variable}}")
    # Map output is owned by MAICORD_GENERATE_LINK_MAPS, not stale cache flags,
    # and /INCREMENTAL:NO by the link contract.
    string(REGEX REPLACE "(^|[ \t])[-/]([Mm][Aa][Pp]|INCREMENTAL:NO)([ \t]|$)" " "
           clean_flags "${clean_flags}")
    string(STRIP "${clean_flags}" clean_flags)
    set(${flag_variable} "${clean_flags}" CACHE STRING "Release linker flags" FORCE)
endforeach()
# Every image states its /SUBSYSTEM (with the minimum version) itself.
foreach(lang C CXX)
    set(CMAKE_${lang}_CREATE_WIN32_EXE "")
    set(CMAKE_${lang}_CREATE_CONSOLE_EXE "")
endforeach()

# One policy owns every final PE's freestanding, mitigation, dead-code and
# link contract. Archive/object reproducibility does not add a hash directory
# to the final PE; omit LINK's /Brepro, which does add that directory.
string(APPEND CMAKE_STATIC_LINKER_FLAGS_RELEASE " /Brepro")

add_library(maicord_link_contract INTERFACE)
target_link_options(maicord_link_contract INTERFACE
    /NODEFAULTLIB
    /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
    /OPT:REF /OPT:ICF=8 /INCREMENTAL:NO
    $<$<CONFIG:Release>:/DEBUG:NONE>
    # /DEBUG:NONE still emits LTCG group and volatile-access diagnostics.
    # Suppress those records while retaining unwind and CET metadata.
    $<$<CONFIG:Release>:/NOCOFFGRPINFO>
    $<$<CONFIG:Release>:/EMITVOLATILEMETADATA:NO>
    $<$<CONFIG:Release>:/EMITTOOLVERSIONINFO:NO>
    $<$<CONFIG:Release>:/NOVCFEATURE>
    # Temporary test targets set MAICORD_TEST_CODEGEN and link without LTCG
    # (see maicord_add_test_codegen_twins); shipping targets always use it.
    $<$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_TEST_CODEGEN>>>:/LTCG>)

add_library(maicord_compile_contract INTERFACE)
target_link_libraries(maicord_compile_contract INTERFACE maicord_link_contract)
target_include_directories(maicord_compile_contract INTERFACE include)
# /X: no INCLUDE environment or standard include paths. The Windows SDK and
# compiler declarations MaiCord uses are its own (include/maicord/sys, with
# C library names in sys/libc). A target with MAICORD_SDK_INCLUDES (the SDK
# conformance units, the reference oracles) compiles against the SDK instead.
set(maicord_sdk_includes "$<BOOL:$<TARGET_PROPERTY:MAICORD_SDK_INCLUDES>>")
target_include_directories(maicord_compile_contract INTERFACE
    "$<$<NOT:${maicord_sdk_includes}>:${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/sys/libc>")
target_compile_definitions(maicord_compile_contract INTERFACE "$<${maicord_sdk_includes}:MAICORD_SDK_INCLUDES>")
target_compile_options(maicord_compile_contract INTERFACE
    "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:${maicord_sdk_includes}>>:/X>")
target_compile_features(maicord_compile_contract INTERFACE cxx_std_23)
target_compile_definitions(maicord_compile_contract INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00)
target_compile_options(maicord_compile_contract INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/WX>
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
    # Pin the SDK ReadAcquire/WriteRelease semantics; this is already the x64 default.
    $<$<COMPILE_LANGUAGE:CXX>:/volatile:ms>
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:CXX>:/Zl>
    $<$<COMPILE_LANGUAGE:CXX>:/GS->
    $<$<COMPILE_LANGUAGE:CXX>:/GR->
    $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->
    $<$<COMPILE_LANGUAGE:CXX>:/fp:fast>
    $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:inline>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:threadSafeInit->
    $<$<COMPILE_LANGUAGE:CXX>:/Gy>
    $<$<COMPILE_LANGUAGE:CXX>:/Gw>
    $<$<COMPILE_LANGUAGE:CXX>:/GF>
    $<$<COMPILE_LANGUAGE:CXX>:/Brepro>
    $<$<COMPILE_LANGUAGE:ASM_MASM>:/Brepro>)
target_compile_options(maicord_compile_contract INTERFACE
    # MAICORD_INLINE_ANY: /Ob2 instead. MAICORD_NO_WHOLE_PROGRAM: no /GL, for
    # code kept outside LTCG in an image that links with it.
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2;/Ob$<IF:$<BOOL:$<TARGET_PROPERTY:MAICORD_INLINE_ANY>>,2,1>>"
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>,$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_TEST_CODEGEN>>>,$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_NO_WHOLE_PROGRAM>>>>:/GL>)

add_library(maicord_runtime OBJECT
    src/core/runtime_x64.asm)
target_link_libraries(maicord_runtime PRIVATE maicord_compile_contract)
set_target_properties(maicord_runtime PROPERTIES CXX_EXTENSIONS OFF)

# maicord_library(<name> <source>... [LIBS <library>...] [INCLUDES <dir>...] [DEPENDS <target>...])
# A shipping static library under the compile contract (linked first unless
# LIBS places it).
function(maicord_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "LIBS;INCLUDES;DEPENDS")
    if(NOT "maicord_compile_contract" IN_LIST ARG_LIBS)
        list(PREPEND ARG_LIBS maicord_compile_contract)
    endif()
    add_library(${name} STATIC ${ARG_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
    if(ARG_INCLUDES)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    endif()
    if(ARG_DEPENDS)
        add_dependencies(${name} ${ARG_DEPENDS})
    endif()
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
endfunction()

# maicord_program(<target> [SHARED|WIN32] ENTRY <symbol> SOURCES <file>...
#     [LIBS <library>...] [INCLUDES <dir>...] [DEFINES <definition>...]
#     [OPTIONS <option>...] [LINK_OPTIONS <option>...] [PROPERTIES <name> <value>...]
#     [DEPENDS <target>...])
# A freestanding tool, test or fixture image: its sources plus the runtime
# helper objects, the compile contract (first unless LIBS places it), /ENTRY
# and a console subsystem (windows for WIN32 executables and SHARED DLLs).
function(maicord_program target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "SHARED;WIN32" "ENTRY"
        "SOURCES;LIBS;INCLUDES;DEFINES;OPTIONS;LINK_OPTIONS;PROPERTIES;DEPENDS")
    if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_ENTRY)
        message(FATAL_ERROR "maicord_program(${target}): needs ENTRY; unexpected: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    set(subsystem CONSOLE)
    if(ARG_SHARED)
        add_library(${target} SHARED ${ARG_SOURCES} $<TARGET_OBJECTS:maicord_runtime>)
        set(subsystem WINDOWS)
    elseif(ARG_WIN32)
        add_executable(${target} WIN32 ${ARG_SOURCES} $<TARGET_OBJECTS:maicord_runtime>)
        set(subsystem WINDOWS)
    else()
        add_executable(${target} ${ARG_SOURCES} $<TARGET_OBJECTS:maicord_runtime>)
    endif()
    if(NOT "maicord_compile_contract" IN_LIST ARG_LIBS)
        list(PREPEND ARG_LIBS maicord_compile_contract)
    endif()
    target_link_libraries(${target} PRIVATE ${ARG_LIBS})
    if(ARG_INCLUDES)
        target_include_directories(${target} PRIVATE ${ARG_INCLUDES})
    endif()
    if(ARG_DEFINES)
        target_compile_definitions(${target} PRIVATE ${ARG_DEFINES})
    endif()
    if(ARG_OPTIONS)
        target_compile_options(${target} PRIVATE ${ARG_OPTIONS})
    endif()
    target_link_options(${target} PRIVATE /ENTRY:${ARG_ENTRY} /SUBSYSTEM:${subsystem},6.02 ${ARG_LINK_OPTIONS})
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF ${ARG_PROPERTIES})
    if(ARG_DEPENDS)
        add_dependencies(${target} ${ARG_DEPENDS})
    endif()
endfunction()
