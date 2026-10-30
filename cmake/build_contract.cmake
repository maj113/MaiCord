# Compiler and linker contract shared by every MaiCord image: freestanding
# C++23 (no CRT, exceptions or RTTI), Release-only code generation and one
# output directory, the build directory itself.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Every Win32 import is declared by the target that owns it. Suppress CMake's
# generic desktop-library bundle (GDI, spooler, OLE, COM dialogs, and others),
# which MaiCord otherwise asks LINK to scan despite /NODEFAULTLIB and no use.
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_C_STANDARD_LIBRARIES "")

if(NOT WIN32)
    message(FATAL_ERROR "MaiCord targets Windows 10 or newer.")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "MaiCord builds with clang-cl, lld-link and llvm-lib (tools/build.ps1 -LlvmBin).")
endif()

# The contract owns every code generation option: no CMake defaults (/EHsc,
# /O2 /Ob2, /INCREMENTAL:NO) ahead of the per-target ones.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS_RELEASE /DNDEBUG)
set(CMAKE_CXX_FLAGS_RELEASE /DNDEBUG)
set(CMAKE_ASM_MASM_FLAGS_RELEASE "")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "")
set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "")
# Every image states its /SUBSYSTEM (with the minimum version) itself.
foreach(lang C CXX)
    set(CMAKE_${lang}_CREATE_WIN32_EXE "")
    set(CMAKE_${lang}_CREATE_CONSOLE_EXE "")
endforeach()

# Whole-program optimization: full LTO in every shipping object, linked by
# lld-link. Cold code is compiled for size with MAICORD_SIZE_OPTION.
set(MAICORD_WHOLE_PROGRAM_OPTION -flto)
set(MAICORD_SIZE_OPTION /clang:-Oz)

# One policy owns every final PE's freestanding, mitigation, dead-code and
# link contract.
add_library(maicord_link_contract INTERFACE)
target_link_options(maicord_link_contract INTERFACE
    /NODEFAULTLIB
    /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
    /OPT:REF /OPT:ICF=8 /INCREMENTAL:NO
    # No bound imports (nothing binds them since Windows 8); lld-link then also
    # omits the import lookup tables and writes import DLL names without their
    # ".dll", which the loader appends.
    /ALLOWBIND:NO
    /DEBUG:NONE)

add_library(maicord_compile_contract INTERFACE)
target_link_libraries(maicord_compile_contract INTERFACE maicord_link_contract)
target_include_directories(maicord_compile_contract INTERFACE include)
# -nostdinc: no include path but MaiCord's own, neither the SDK's, the INCLUDE
# environment's nor the compiler's resource headers. The Windows SDK, C library
# and compiler declarations MaiCord uses are its own (include/maicord/sys, with
# C library names in sys/libc). A target with MAICORD_SDK_INCLUDES (the SDK
# conformance unit, the reference oracles) compiles against the SDK instead.
set(maicord_sdk_includes "$<BOOL:$<TARGET_PROPERTY:MAICORD_SDK_INCLUDES>>")
target_include_directories(maicord_compile_contract INTERFACE
    "$<$<NOT:${maicord_sdk_includes}>:${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/sys/libc>")
target_compile_definitions(maicord_compile_contract INTERFACE "$<${maicord_sdk_includes}:MAICORD_SDK_INCLUDES>")
target_compile_options(maicord_compile_contract INTERFACE
    "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:${maicord_sdk_includes}>>:/clang:-nostdinc>")
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
    # Strict IEEE semantics: text metrics and hit tests are bit-exact and NaN and
    # infinity guards hold.
    $<$<COMPILE_LANGUAGE:CXX>:/fp:precise>
    $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:inline>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:threadSafeInit->
    $<$<COMPILE_LANGUAGE:CXX>:/Gy>
    $<$<COMPILE_LANGUAGE:CXX>:/Gw>
    $<$<COMPILE_LANGUAGE:CXX>:/GF>
    $<$<COMPILE_LANGUAGE:CXX>:/Brepro>
    $<$<COMPILE_LANGUAGE:ASM_MASM>:/Brepro>)
target_compile_options(maicord_compile_contract INTERFACE
    # MAICORD_INLINE_ANY: /Ob2 instead. MAICORD_NO_WHOLE_PROGRAM: no LTO, for
    # code kept outside it in an image that links with it.
    "$<$<COMPILE_LANGUAGE:CXX>:/O2;/Ob$<IF:$<BOOL:$<TARGET_PROPERTY:MAICORD_INLINE_ANY>>,2,1>>"
    $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_TEST_CODEGEN>>>,$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_NO_WHOLE_PROGRAM>>>>:${MAICORD_WHOLE_PROGRAM_OPTION}>
    # Compact C++ EH metadata (FH4).
    $<$<COMPILE_LANGUAGE:CXX>:/d2FH4>
    # Diagnostics beyond the house /W4 set: on declarations that mirror the SDK
    # (uuid typedefs, hidden COM overloads, extern "C" extern), on intended
    # aggregate value-initialization and layout asserts, and layout heuristics
    # for the dense one-line style (indentation, && in ||, elided subobject
    # braces) and deliberate non-short-circuit bool |.
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers;-Wno-invalid-offsetof;-Wno-ignored-attributes;-Wno-overloaded-virtual;-Wno-duplicate-decl-specifier;-Wno-misleading-indentation;-Wno-logical-op-parentheses;-Wno-missing-braces;-Wno-bitwise-instead-of-logical>"
    # No C library: LLVM must not turn string-length loops into strlen/wcslen calls.
    "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-builtin-strlen;-fno-builtin-wcslen>")
# No unwind tables (.pdata/.xdata): MaiCord code never throws, catches or walks
# a stack. A crash still ends the process (Windows Error Reporting, __fastfail),
# but frame-based exception dispatch stops at the first MaiCord frame, so
# unhandled-exception filters never run (the tests report crashes from a
# vectored handler) and debuggers walk crash-dump stacks by heuristics. The
# in-process stack walkers (the perf probe's sampler and allocation sites,
# benchmark profiling) configure with MAICORD_UNWIND_TABLES.
option(MAICORD_UNWIND_TABLES "Emit unwind tables for in-process stack walkers" OFF)
set(MAICORD_UNWIND_OPTIONS)
if(NOT MAICORD_UNWIND_TABLES)
    set(MAICORD_UNWIND_OPTIONS /clang:-fno-asynchronous-unwind-tables /clang:-fno-unwind-tables)
endif()
target_compile_options(maicord_compile_contract INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${MAICORD_UNWIND_OPTIONS}>")

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
# A freestanding image: its sources plus the runtime
# helper objects, the compile contract (first unless LIBS places it), /ENTRY
# and a console subsystem (windows for WIN32 executables and SHARED DLLs).
function(maicord_program target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "SHARED;WIN32" "ENTRY"
        "SOURCES;LIBS;INCLUDES;DEFINES;OPTIONS;LINK_OPTIONS;PROPERTIES;DEPENDS")
    if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_ENTRY)
        message(FATAL_ERROR "maicord_program(${target}): needs ENTRY; unexpected: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    # Preserve explicit runtime placement in a shipping target's source list.
    if(NOT "$<TARGET_OBJECTS:maicord_runtime>" IN_LIST ARG_SOURCES)
        list(APPEND ARG_SOURCES $<TARGET_OBJECTS:maicord_runtime>)
    endif()
    set(subsystem CONSOLE)
    if(ARG_SHARED)
        add_library(${target} SHARED ${ARG_SOURCES})
        set(subsystem WINDOWS)
    elseif(ARG_WIN32)
        add_executable(${target} WIN32 ${ARG_SOURCES})
        set(subsystem WINDOWS)
    else()
        add_executable(${target} ${ARG_SOURCES})
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
