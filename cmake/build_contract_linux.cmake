# The Linux flavour of the compiler and linker contract (cmake/build_contract.cmake is the Windows one; this file is
# included from it): the same freestanding C++23 (no libc, exceptions or RTTI), Release-only code generation and
# one output directory, built by clang and ld.lld: the pinned maj113/llvm-pe release (tools/build_linux.sh), or a stock
# LLVM, which only lacks the options maicord_llvm_options checks for. The names the rest of the build uses
# (maicord_link_contract, maicord_compile_contract, maicord_runtime, maicord_library, maicord_program) are the same.
#
# An image is a dynamic position-independent executable with its own _start (src/core/linux/start.cpp). It links
# libc.so.6 as a generated stub library that pins the oldest symbol versions glibc 2.34's libc.so.6 exports
# (tools/generate_libc_stubs.py), so no glibc newer than 2.34 is ever required (dlopen and pthread live in libc.so.6
# since then, and the Linux floor admits no older glibc); everything else is dlopen()ed. The same image is the musl one
# once tools/retarget_libc.py has changed its program interpreter (src/core/linux/README.md). tools/audit_elf.py checks
# the result.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_C_STANDARD_LIBRARIES "")

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    message(FATAL_ERROR "MaiCord on Linux targets x86-64.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "MaiCord builds with clang and ld.lld (tools/build_linux.sh).")
endif()
# -flto objects are LLVM bitcode, which only llvm-ar can index.
if(NOT CMAKE_AR MATCHES "llvm-ar")
    message(FATAL_ERROR "MaiCord archives with llvm-ar (CMAKE_AR=${CMAKE_AR}); tools/build_linux.sh sets it.")
endif()

# The contract owns every code generation option: no CMake defaults (-O3, -fPIE or -pie of its own).
set(CMAKE_C_FLAGS_RELEASE -DNDEBUG)
set(CMAKE_CXX_FLAGS_RELEASE -DNDEBUG)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "")
set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "")

# Whole-program optimization: full LTO in every shipping object, linked by ld.lld.
set(MAICORD_WHOLE_PROGRAM_OPTION -flto)
# Machine outlining (enabled at link time below) trades call overhead for size and slows speed-compiled code, so only
# sources compiled for size are outlined: the contract's -mno-outline marks every function nooutline and the size
# option's -moutline lifts it again (the Windows contract's policy, cmake/build_contract.cmake).
set(MAICORD_NO_OUTLINE_OPTION -mno-outline)
set(MAICORD_SIZE_OPTION -Oz -moutline)
# The size-compiled sources that fold well take -finline-functions over the contract's -finline-hint-functions
# (cmake/size_policy.cmake): the contract's hint-only inlining marks every function not declared inline noinline,
# so the helpers of a -Oz source stay calls even where inlining them is smaller.
set(MAICORD_FOLD_OPTION -finline-functions)
# Between the two: sources the Windows build leaves at -O2 compile -Os on Linux unless a measurement shows them hot
# (cmake/size_policy.cmake, maicord_source_options): inlining and unrolling are limited and nothing is vectorized, but
# nothing is outlined either (-moutline stays with the size option: outlining the -Os group slowed the stores and text
# entry). The Windows contract has no such level.
set(MAICORD_BALANCED_OPTION -Os)

# The libc stub: libc.so.6 carrying the pinned symbol versions.
set(MAICORD_LIBC_STUB_DIRECTORY "${CMAKE_BINARY_DIR}/libc-stubs")
set(maicord_libc_stub_files "${MAICORD_LIBC_STUB_DIRECTORY}/libc.so.6")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT ${maicord_libc_stub_files}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_libc_stubs.py"
        --compiler "${CMAKE_C_COMPILER}" --output-directory "${MAICORD_LIBC_STUB_DIRECTORY}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_libc_stubs.py"
    COMMENT "Generating the libc stub library"
    VERBATIM)
add_custom_target(maicord_libc_stubs DEPENDS ${maicord_libc_stub_files})

# One policy owns every final image's freestanding, mitigation, dead-code and link contract.
add_library(maicord_link_contract INTERFACE)
target_link_options(maicord_link_contract INTERFACE
    -fuse-ld=lld -nostdlib -pie
    LINKER:-e,_start
    LINKER:--dynamic-linker=/lib64/ld-linux-x86-64.so.2
    LINKER:-z,relro LINKER:-z,now LINKER:-z,noexecstack
    LINKER:--build-id=none
    LINKER:--gc-sections LINKER:--icf=all
    # Relative relocations packed as RELR under the Android tags, which no glibc knows: src/core/linux/start.cpp
    # applies them itself, so one image runs on every glibc from 2.34 (DT_RELR needs 2.36 and a version dependency).
    LINKER:--pack-dyn-relocs=relr LINKER:--use-android-relr-tags
    LINKER:--no-undefined
    "-L${MAICORD_LIBC_STUB_DIRECTORY}"
    -l:libc.so.6)

add_library(maicord_compile_contract INTERFACE)
target_link_libraries(maicord_compile_contract INTERFACE maicord_link_contract)
target_include_directories(maicord_compile_contract INTERFACE include)
# -nostdinc: no include path but MaiCord's own, neither the system's nor the compiler's resource headers. The C
# library and compiler declarations MaiCord uses are its own (include/maicord/sys, with C library names in sys/libc
# and the kernel and glibc interface in sys/linux).
target_include_directories(maicord_compile_contract INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/sys/libc")
target_compile_options(maicord_compile_contract INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-nostdinc>")
target_compile_features(maicord_compile_contract INTERFACE cxx_std_23)
target_compile_options(maicord_compile_contract INTERFACE
    # Freestanding by construction (the headers are MaiCord's own, nothing is hosted), but not -ffreestanding: it
    # implies -fno-builtin, which would turn every memcpy of a constant size into a call. Only the string-length
    # builtins are off, as on Windows: LLVM must not turn string-length loops into strlen/wcslen calls.
    "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-builtin-strlen;-fno-builtin-wcslen;-fno-math-errno;${MAICORD_NO_OUTLINE_OPTION}>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-fno-threadsafe-statics>"
    # MaiCord's shared code is written for MSVC dialect: __declspec, __forceinline, __unaligned, __int64 and
    # volatile with acquire/release semantics, and wchar_t holds UTF-16 code units as on Windows.
    "$<$<COMPILE_LANGUAGE:C,CXX>:-fms-extensions;-fms-volatile;-fshort-wchar>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:-fvisibility=hidden;-ffunction-sections;-fdata-sections;-fPIE>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-stack-protector;-fcf-protection=none>"
    # Strict IEEE semantics, as /fp:precise.
    "$<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>"
    # The x86-64 baseline of the Windows build: /arch:AVX and -maes (the carry-less multiply the JSON scanner
    # uses is part of the same baseline there; clang needs it named).
    "$<$<COMPILE_LANGUAGE:CXX>:-mavx;-maes;-mpclmul>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:-O2>" "$<$<COMPILE_LANGUAGE:CXX>:-finline-hint-functions>"
    # MAICORD_NO_WHOLE_PROGRAM: no LTO, for code kept outside it in an image that links with it.
    $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_TEST_CODEGEN>>>,$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_NO_WHOLE_PROGRAM>>>>:${MAICORD_WHOLE_PROGRAM_OPTION}>
    # The house warning set (clang-cl /W4 /WX), with the exceptions of the Windows contract: declarations that
    # mirror an interface, intended aggregate value-initialization and layout asserts, the dense one-line style
    # and deliberate non-short-circuit bool |.
    "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers;-Wno-invalid-offsetof;-Wno-ignored-attributes;-Wno-overloaded-virtual;-Wno-duplicate-decl-specifier;-Wno-misleading-indentation;-Wno-logical-op-parentheses;-Wno-missing-braces;-Wno-bitwise-instead-of-logical>")
# clang 24 reports #embed in C++ under -Wc++26-extensions (clang 23 under -Wc23-extensions, which the targets that
# #embed turn off themselves); the Windows sources that #embed silence it around the directive.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wno-c++26-extensions MAICORD_CXX_NO_CXX26_EXTENSIONS_WARNING)
if(MAICORD_CXX_NO_CXX26_EXTENSIONS_WARNING)
    target_compile_options(maicord_compile_contract INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-Wno-c++26-extensions>")
endif()
# No unwind tables: MaiCord code never throws, catches or walks a stack. The in-process stack walkers configure
# with MAICORD_UNWIND_TABLES.
option(MAICORD_UNWIND_TABLES "Emit unwind tables for in-process stack walkers" OFF)
if(NOT MAICORD_UNWIND_TABLES)
    target_compile_options(maicord_compile_contract INTERFACE
        "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-asynchronous-unwind-tables;-fno-unwind-tables>")
    # The machine outliner (see MAICORD_SIZE_OPTION) emits no unwind data for the functions it creates, so stack
    # walkers get none.
    target_link_options(maicord_link_contract INTERFACE LINKER:--mllvm=-enable-machine-outliner)
    # IPRA: a caller keeps values in the scratch registers its callees leave alone. Stock LLVM's is sound on SysV, which
    # has no callee-saved vector registers (the Windows contract explains why Win64 takes only the fork's), and takes
    # 100 KB off the client; the fork's -x86-ipra-nocsr=false, which keeps it to the register masks, 17 KB more.
    maicord_llvm_options(MAICORD_LLVM_IPRA_MASKS -enable-ipra -x86-ipra-nocsr=false)
    if(NOT MAICORD_LLVM_IPRA_MASKS)
        maicord_llvm_options(MAICORD_LLVM_IPRA -enable-ipra)
    endif()
    # The fork's: functions compiled for speed are not aligned to 16 bytes either (6 KB).
    maicord_llvm_options(MAICORD_LLVM_FUNCTION_ALIGNMENT -x86-function-alignment=1)
endif()

# The compiler support every image links: the memory functions and the process entry.
add_library(maicord_runtime OBJECT
    src/core/linux/runtime.cpp
    src/core/linux/start.cpp)
target_link_libraries(maicord_runtime PRIVATE maicord_compile_contract)
set_target_properties(maicord_runtime PROPERTIES CXX_EXTENSIONS OFF)
# The bodies of memcpy and the others must not be turned back into calls of themselves.
set_source_files_properties(src/core/linux/runtime.cpp PROPERTIES COMPILE_OPTIONS -fno-builtin)

# maicord_program(<target> ENTRY <symbol> SOURCES <file>... [LIBS <library>...] [INCLUDES <dir>...]
#     [DEFINES <definition>...] [OPTIONS <option>...] [LINK_OPTIONS <option>...] [PROPERTIES <name> <value>...]
#     [DEPENDS <target>...])
# A freestanding image: its sources plus the runtime objects, the compile contract (first unless LIBS places it)
# and the entry: _start calls the extern "C" function ENTRY, which ends the process (exit_group).
function(maicord_program target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ENTRY"
        "SOURCES;LIBS;INCLUDES;DEFINES;OPTIONS;LINK_OPTIONS;PROPERTIES;DEPENDS")
    if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_ENTRY)
        message(FATAL_ERROR "maicord_program(${target}): needs ENTRY; unexpected: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT "$<TARGET_OBJECTS:maicord_runtime>" IN_LIST ARG_SOURCES)
        list(APPEND ARG_SOURCES $<TARGET_OBJECTS:maicord_runtime>)
    endif()
    add_executable(${target} ${ARG_SOURCES})
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
    target_link_options(${target} PRIVATE LINKER:--defsym=maicord_program_entry=${ARG_ENTRY} ${ARG_LINK_OPTIONS})
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF ${ARG_PROPERTIES})
    add_dependencies(${target} maicord_libc_stubs)
    if(ARG_DEPENDS)
        add_dependencies(${target} ${ARG_DEPENDS})
    endif()
endfunction()
