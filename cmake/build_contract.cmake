# Compiler and linker contract shared by every MaiCord image on both systems: freestanding C++23 (no C library,
# exceptions or RTTI), Release-only code generation and one output directory, the build directory itself. Windows builds
# with clang-cl, lld-link and llvm-lib, Linux with clang, ld.lld and llvm-ar: the pinned maj113/llvm-pe release, or a
# stock LLVM, which only lacks the options maicord_llvm_options checks for. The policy is one; only the drivers' spelling
# (clang-cl takes clang's own options as /clang:, maicord_clang), the image formats, the C library and the runtime
# differ.
#
# A Linux image is a dynamic position-independent executable with its own _start (src/core/linux/start.cpp). It links
# libc.so.6 as a generated stub library that pins the oldest symbol versions glibc 2.34's libc.so.6 exports
# (tools/generate_libc_stubs.py), so no glibc newer than 2.34 is ever required (dlopen and pthread live in libc.so.6
# since then, and the Linux floor admits no older glibc); everything else is dlopen()ed. The same image is the musl one
# once tools/retarget_libc.py has changed its program interpreter (src/core/linux/README.md). tools/audit_image.py checks
# the images of both systems.

# maicord_library entries prefixed windows: or linux: (sources, LIBS, INCLUDES, DEPENDS) apply on that system only, in
# place, so one list keeps each system's archive member order (cmake/libraries.cmake).
if(MAICORD_LINUX)
    set(maicord_system linux)
    set(maicord_other_system windows)
    set(maicord_clang "")
else()
    set(maicord_system windows)
    set(maicord_other_system linux)
    set(maicord_clang /clang:)
endif()

# maicord_library(<name> <source>... [LIBS <library>...] [INCLUDES <dir>...] [DEPENDS <target>...])
# A shipping static library under the compile contract (linked first unless LIBS places it).
function(maicord_library name)
    set(arguments ${ARGN})
    list(FILTER arguments EXCLUDE REGEX "^${maicord_other_system}:")
    list(TRANSFORM arguments REPLACE "^${maicord_system}:" "")
    cmake_parse_arguments(ARG "" "" "LIBS;INCLUDES;DEPENDS" ${arguments})
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

# maicord_llvm_options(<variable> <option>...)
# Adds LLVM code generation options (-mllvm) to the link contract, for the LTO code generation, and to the compile
# contract, for the objects outside it (the tests' non-LTO twins), when the toolchain's compiler and linker both take all
# of them; <variable> caches the answer. The options of the maj113/llvm-pe fork come in this way, so a stock LLVM, which
# rejects them, builds without them.
include(CheckCXXSourceCompiles)
function(maicord_llvm_options variable)
    if(MSVC)
        list(TRANSFORM ARGN PREPEND /clang:-mllvm= OUTPUT_VARIABLE compile)
        list(TRANSFORM ARGN PREPEND /mllvm: OUTPUT_VARIABLE link)
        set(CMAKE_REQUIRED_LINK_OPTIONS ${link})
    else()
        list(TRANSFORM ARGN PREPEND -mllvm= OUTPUT_VARIABLE compile)
        list(TRANSFORM ARGN PREPEND LINKER:--mllvm= OUTPUT_VARIABLE link)
        # A try-compile that only archives (CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY, for a clang that cannot link
        # a hosted program: packaging/README.md) checks the compiler alone; ld.lld is the same LLVM.
        if(NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
            set(CMAKE_REQUIRED_LINK_OPTIONS -fuse-ld=lld ${link})
        endif()
    endif()
    list(JOIN compile " " CMAKE_REQUIRED_FLAGS)
    check_cxx_source_compiles("int main() { return 0; }" ${variable})
    if(${variable})
        target_compile_options(maicord_compile_contract INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${compile}>")
        target_link_options(maicord_link_contract INTERFACE ${link})
    endif()
endfunction()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Every system library is declared by the target that owns it. Suppress CMake's
# generic desktop-library bundle (on Windows GDI, spooler, OLE, COM dialogs, and
# others), which the linker would otherwise scan despite /NODEFAULTLIB and no use.
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_C_STANDARD_LIBRARIES "")

if(MAICORD_LINUX)
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
elseif(NOT WIN32)
    message(FATAL_ERROR "MaiCord targets Windows 10 version 1809 or newer and Linux.")
elseif(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "MaiCord builds with clang-cl, lld-link and llvm-lib (tools/build.ps1 -LlvmBin).")
endif()

# The contract owns every code generation option: no CMake defaults (/EHsc, /O2 /Ob2 and /INCREMENTAL:NO on Windows,
# -O3, -fPIE or -pie on Linux) ahead of the per-target ones.
if(MAICORD_LINUX)
    set(CMAKE_C_FLAGS_RELEASE -DNDEBUG)
    set(CMAKE_CXX_FLAGS_RELEASE -DNDEBUG)
else()
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_C_FLAGS_RELEASE /DNDEBUG)
    set(CMAKE_CXX_FLAGS_RELEASE /DNDEBUG)
    set(CMAKE_ASM_MASM_FLAGS_RELEASE "")
    # Every image states its /SUBSYSTEM (with the minimum version) itself.
    foreach(lang C CXX)
        set(CMAKE_${lang}_CREATE_WIN32_EXE "")
        set(CMAKE_${lang}_CREATE_CONSOLE_EXE "")
    endforeach()
endif()
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "")
set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "")

# Whole-program optimization: full LTO in every shipping object, linked by lld.
# Cold code is compiled for size with MAICORD_SIZE_OPTION.
set(MAICORD_WHOLE_PROGRAM_OPTION -flto)
# Machine outlining (enabled at link time below) trades call overhead for size
# and slowed speed-compiled code by 5-60% (zstd, WebP, XChaCha, Opus), so only
# sources compiled for size are outlined: the contract's -mno-outline marks
# every function nooutline and the size option's -moutline lifts it again.
set(MAICORD_NO_OUTLINE_OPTION ${maicord_clang}-mno-outline)
set(MAICORD_SIZE_OPTION ${maicord_clang}-Oz ${maicord_clang}-moutline)
# The size-compiled sources that fold well take MAICORD_FOLD_OPTION over the contract's inlining (cmake/size_policy.cmake):
# /Ob2 over /Ob1 on Windows; on Linux -finline-functions over -finline-hint-functions, whose hint-only inlining marks
# every function not declared inline noinline, so the helpers of a -Oz source stay calls even where inlining them is
# smaller.
if(MAICORD_LINUX)
    set(MAICORD_FOLD_OPTION -finline-functions)
    # Between the two: sources the Windows build leaves at -O2 compile -Os on Linux unless a measurement shows them hot
    # (cmake/size_policy.cmake, maicord_source_options): inlining and unrolling are limited and nothing is vectorized,
    # but nothing is outlined either (-moutline stays with the size option: outlining the -Os group slowed the stores
    # and text entry). Windows has no such level.
    set(MAICORD_BALANCED_OPTION -Os)
else()
    set(MAICORD_FOLD_OPTION /Ob2)
endif()

# One policy owns every final image's freestanding, mitigation, dead-code and
# link contract.
add_library(maicord_link_contract INTERFACE)
if(MAICORD_LINUX)
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
else()
    target_link_options(maicord_link_contract INTERFACE
        /NODEFAULTLIB
        /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
        /OPT:REF /OPT:ICF=8 /INCREMENTAL:NO
        # No bound imports (nothing binds them since Windows 8); lld-link then also
        # omits the import lookup tables and writes import DLL names without their
        # ".dll", which the loader appends.
        /ALLOWBIND:NO
        # The only unwind data is the delay-load thunks' 120 bytes: keep it in
        # .rdata instead of a section of its own (512 file bytes).
        /MERGE:.pdata=.rdata
        /DEBUG:NONE)
endif()

add_library(maicord_compile_contract INTERFACE)
target_link_libraries(maicord_compile_contract INTERFACE maicord_link_contract)
target_include_directories(maicord_compile_contract INTERFACE include)
# -nostdinc: no include path but MaiCord's own, neither the system's (on Windows
# the SDK's and the INCLUDE environment's) nor the compiler's resource headers.
# The system, C library and compiler declarations MaiCord uses are its own
# (include/maicord/sys, with C library names in sys/libc and the kernel and glibc
# interface in sys/linux). A Windows target with MAICORD_SDK_INCLUDES (the SDK
# conformance unit, the reference oracles) compiles against the SDK instead.
set(maicord_sdk_includes "$<BOOL:$<TARGET_PROPERTY:MAICORD_SDK_INCLUDES>>")
target_include_directories(maicord_compile_contract INTERFACE
    "$<$<NOT:${maicord_sdk_includes}>:${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/sys/libc>")
target_compile_definitions(maicord_compile_contract INTERFACE "$<${maicord_sdk_includes}:MAICORD_SDK_INCLUDES>")
target_compile_options(maicord_compile_contract INTERFACE
    "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:${maicord_sdk_includes}>>:${maicord_clang}-nostdinc>")
target_compile_features(maicord_compile_contract INTERFACE cxx_std_23)
# Diagnostics beyond the house set (/W4 /WX, -Wall -Wextra -Werror): on
# declarations that mirror an interface (uuid typedefs, hidden COM overloads,
# extern "C" extern), on intended aggregate value-initialization and layout
# asserts, and layout heuristics for the dense one-line style (indentation, &&
# in ||, elided subobject braces) and deliberate non-short-circuit bool |.
set(maicord_warning_exceptions -Wno-missing-field-initializers -Wno-invalid-offsetof -Wno-ignored-attributes
    -Wno-overloaded-virtual -Wno-duplicate-decl-specifier -Wno-misleading-indentation -Wno-logical-op-parentheses
    -Wno-missing-braces -Wno-bitwise-instead-of-logical)
if(MAICORD_LINUX)
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
        # The x86-64 baseline of the Windows build: clang-cl's /arch:AVX is Sandy Bridge (AVX, POPCNT, PCLMULQDQ,
        # CMPXCHG16B) with generic tuning, plus -maes. Sandy Bridge's own tuning grew the client by 38 KB.
        "$<$<COMPILE_LANGUAGE:CXX>:-march=sandybridge;-mtune=generic;-maes>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-O2>" "$<$<COMPILE_LANGUAGE:CXX>:-finline-hint-functions>"
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_TEST_CODEGEN>>>>:${MAICORD_WHOLE_PROGRAM_OPTION}>"
        "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>"
        "$<$<COMPILE_LANGUAGE:CXX>:${maicord_warning_exceptions}>")
    # clang 24 reports #embed in C++ under -Wc++26-extensions (clang 23 under -Wc23-extensions, which the targets that
    # #embed turn off themselves); the Windows sources that #embed silence it around the directive.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wno-c++26-extensions MAICORD_CXX_NO_CXX26_EXTENSIONS_WARNING)
    if(MAICORD_CXX_NO_CXX26_EXTENSIONS_WARNING)
        target_compile_options(maicord_compile_contract INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-Wno-c++26-extensions>")
    endif()
else()
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
        $<$<COMPILE_LANGUAGE:CXX>:/Zl>
        $<$<COMPILE_LANGUAGE:CXX>:/GS->
        $<$<COMPILE_LANGUAGE:CXX>:/GR->
        $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->
        # Strict IEEE semantics: text metrics and hit tests are bit-exact and NaN and
        # infinity guards hold.
        $<$<COMPILE_LANGUAGE:CXX>:/fp:precise>
        $<$<COMPILE_LANGUAGE:CXX>:/arch:AVX>
        $<$<COMPILE_LANGUAGE:CXX>:-maes>
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:threadSafeInit->
        $<$<COMPILE_LANGUAGE:CXX>:/Gy>
        $<$<COMPILE_LANGUAGE:CXX>:/Gw>
        $<$<COMPILE_LANGUAGE:CXX>:/GF>
        $<$<COMPILE_LANGUAGE:CXX>:/Brepro>
        $<$<COMPILE_LANGUAGE:ASM_MASM>:/Brepro>)
    target_compile_options(maicord_compile_contract INTERFACE
        # MAICORD_INLINE_ANY: /Ob2 instead.
        "$<$<COMPILE_LANGUAGE:CXX>:/O2;/Ob$<IF:$<BOOL:$<TARGET_PROPERTY:MAICORD_INLINE_ANY>>,2,1>>"
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<BOOL:$<TARGET_PROPERTY:MAICORD_TEST_CODEGEN>>>>:${MAICORD_WHOLE_PROGRAM_OPTION}>
        # Compact C++ EH metadata (FH4).
        $<$<COMPILE_LANGUAGE:CXX>:/d2FH4>
        "$<$<COMPILE_LANGUAGE:CXX>:${maicord_warning_exceptions}>"
        # No C library: LLVM must not turn string-length loops into strlen/wcslen calls.
        "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-builtin-strlen;-fno-builtin-wcslen;${MAICORD_NO_OUTLINE_OPTION}>")
endif()
# No unwind tables (.pdata/.xdata on Windows): MaiCord code never throws, catches
# or walks a stack. A crash still ends the process (Windows Error Reporting,
# __fastfail), but frame-based exception dispatch stops at the first MaiCord
# frame, so unhandled-exception filters never run (the tests report crashes from
# a vectored handler) and debuggers walk crash-dump stacks by heuristics. The
# in-process stack walkers (the perf probe's sampler and allocation sites)
# configure with MAICORD_UNWIND_TABLES. MAICORD_UNWIND_OPTIONS are the compile
# options, also for the targets outside the contract (maiopus).
option(MAICORD_UNWIND_TABLES "Emit unwind tables for in-process stack walkers" OFF)
set(MAICORD_UNWIND_OPTIONS)
if(NOT MAICORD_UNWIND_TABLES)
    set(MAICORD_UNWIND_OPTIONS ${maicord_clang}-fno-asynchronous-unwind-tables ${maicord_clang}-fno-unwind-tables)
    # The machine outliner (see MAICORD_SIZE_OPTION) emits no unwind data for
    # the functions it creates, so stack walkers get none.
    if(MAICORD_LINUX)
        target_compile_options(maicord_compile_contract INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${MAICORD_UNWIND_OPTIONS}>")
        target_link_options(maicord_link_contract INTERFACE LINKER:--mllvm=-enable-machine-outliner)
        # IPRA: a caller keeps values in the scratch registers its callees leave alone. Stock LLVM's is sound on SysV,
        # which has no callee-saved vector registers (the Windows branch explains why Win64 takes only the fork's), and
        # takes 100 KB off the client; the fork's -x86-ipra-nocsr=false, which keeps it to the register masks, 17 KB more.
        maicord_llvm_options(MAICORD_LLVM_IPRA_MASKS -enable-ipra -x86-ipra-nocsr=false)
        if(NOT MAICORD_LLVM_IPRA_MASKS)
            maicord_llvm_options(MAICORD_LLVM_IPRA -enable-ipra)
        endif()
        # The fork's: functions compiled for speed are not aligned to 16 bytes either (6 KB).
        maicord_llvm_options(MAICORD_LLVM_FUNCTION_ALIGNMENT -x86-function-alignment=1)
    else()
        # Shrink wrapping (prologue and epilogue placed around the code that needs
        # them, so early exits skip the pushes and the frame) is off for every
        # Windows target because its unwind codes describe one prologue in the entry
        # block. Without unwind tables nothing reads them, so turn it on: the link
        # option for the LTO code generation, the compile one for the objects
        # outside it.
        target_link_options(maicord_link_contract INTERFACE /mllvm:-enable-machine-outliner /mllvm:-enable-shrink-wrap=true)
        list(APPEND MAICORD_UNWIND_OPTIONS /clang:-mllvm=-enable-shrink-wrap=true)
        # The code generation of the maj113/llvm-pe fork, each part only when the
        # toolchain takes it (maicord_llvm_options; a stock LLVM builds without
        # them). Together they take about 240 KB (3.7%) of .text off the client,
        # with speed within noise or better.
        # The internal call contract: a function that only its module calls, and
        # only directly, takes six integer and six vector arguments in registers
        # and no home area. It leaves functions with unwind tables alone.
        maicord_llvm_options(MAICORD_LLVM_INTERNAL_CC -x86-internal-cc)
        # IPRA on register masks: a caller keeps values in the scratch registers
        # its callees leave alone. Only the fork's: stock IPRA counts XMM6-15 as
        # clobbered by every vzeroupper (+520 KB) and YMM6-15 as preserved by a
        # callee that saves only their low halves (unsound), and its mode without
        # callee-saved registers grows the client. -x86-ipra-nocsr=false, which
        # only the fork knows, keeps it to the masks.
        maicord_llvm_options(MAICORD_LLVM_IPRA_MASKS -enable-ipra -x86-ipra-nocsr=false)
        # Functions compiled for speed are not aligned to 16 bytes either.
        maicord_llvm_options(MAICORD_LLVM_FUNCTION_ALIGNMENT -x86-function-alignment=1)
        # lld-link's branch relaxation: a tail jump to the next function goes away
        # and a near one becomes a short jump. lld-link refuses it with /debug,
        # which CMake's own try-compile links use, so it is a link contract option
        # only and its check links as Release.
        include(CheckLinkerFlag)
        block()
            set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
            check_linker_flag(CXX /OPT:LLDBRANCHRELAX MAICORD_LLD_BRANCH_RELAX)
        endblock()
        if(MAICORD_LLD_BRANCH_RELAX)
            target_link_options(maicord_link_contract INTERFACE /OPT:LLDBRANCHRELAX)
        endif()
        target_compile_options(maicord_compile_contract INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:${MAICORD_UNWIND_OPTIONS}>")
    endif()
endif()

# The compiler support every image links: the memory functions, and on Linux the process entry.
if(MAICORD_LINUX)
    add_library(maicord_runtime OBJECT src/core/linux/runtime.cpp src/core/linux/start.cpp)
    # The bodies of memcpy and the others must not be turned back into calls of themselves.
    set_source_files_properties(src/core/linux/runtime.cpp PROPERTIES COMPILE_OPTIONS -fno-builtin)
else()
    add_library(maicord_runtime OBJECT src/core/runtime_x64.asm)
endif()
target_link_libraries(maicord_runtime PRIVATE maicord_compile_contract)
set_target_properties(maicord_runtime PROPERTIES CXX_EXTENSIONS OFF)

# maicord_program(<target> [SHARED|WIN32] ENTRY <symbol> SOURCES <file>...
#     [LIBS <library>...] [INCLUDES <dir>...] [DEFINES <definition>...]
#     [OPTIONS <option>...] [LINK_OPTIONS <option>...] [PROPERTIES <name> <value>...]
#     [DEPENDS <target>...])
# A freestanding image: its sources plus the runtime objects, the compile
# contract (first unless LIBS places it) and the entry. On Windows /ENTRY and a
# console subsystem (windows for WIN32 executables and SHARED DLLs); on Linux
# _start calls the extern "C" function ENTRY, which ends the process (exit_group).
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
    if(MAICORD_LINUX)
        target_link_options(${target} PRIVATE LINKER:--defsym=maicord_program_entry=${ARG_ENTRY} ${ARG_LINK_OPTIONS})
        add_dependencies(${target} maicord_libc_stubs)
    else()
        target_link_options(${target} PRIVATE /ENTRY:${ARG_ENTRY} /SUBSYSTEM:${subsystem},6.02 ${ARG_LINK_OPTIONS})
    endif()
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF ${ARG_PROPERTIES})
    if(ARG_DEPENDS)
        add_dependencies(${target} ${ARG_DEPENDS})
    endif()
endfunction()
