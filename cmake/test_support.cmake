# Helpers for the temporary test configuration (cmake/tests.cmake).

# maicord_test(<target> <maicord_program arguments>... [BUNDLE <bundle>] [USES_CLIENT <target>...]
#              [<test properties>] [TEST <name> [<argument>...]]...)
# A test executable or fixture (see maicord_program) and the CTest cases that
# run it. USES_CLIENT, the test properties and the TEST rows come last; they
# are passed to maicord_test_uses_client() and maicord_add_tests(). BUNDLE
# links the sources into the shared executable <bundle> instead (see
# maicord_test_bundle_member); <target> then only names the test, unless
# STANDALONE also builds it on its own for the tools that run it directly.
function(maicord_test target)
    set(program)
    set(client)
    set(tests)
    set(bundle)
    set(part program)
    set(bundle_name FALSE)
    set(standalone FALSE)
    foreach(word IN LISTS ARGN)
        if(bundle_name)
            set(bundle "${word}")
            set(bundle_name FALSE)
            continue()
        elseif(NOT part STREQUAL "tests" AND word MATCHES
                "^(TEST|TIMEOUT|COST|LABELS|RESOURCE_LOCK|ENVIRONMENT|ENV|WORKING_DIRECTORY|PASS_REGULAR_EXPRESSION)$")
            set(part tests)
        elseif(part STREQUAL "program" AND word STREQUAL "USES_CLIENT")
            set(part client)
            continue()
        elseif(part STREQUAL "program" AND word STREQUAL "BUNDLE")
            set(bundle_name TRUE)
            continue()
        elseif(part STREQUAL "program" AND word STREQUAL "STANDALONE")
            set(standalone TRUE)
            continue()
        endif()
        list(APPEND ${part} "${word}")
    endforeach()
    if(bundle)
        maicord_test_bundle_member(${bundle} ${target} ${program})
    endif()
    if(NOT bundle OR standalone)
        maicord_program(${target} ${program})
    endif()
    if(client)
        maicord_test_uses_client(${target} ${client})
    endif()
    if(tests)
        maicord_add_tests(${target} ${tests})
    endif()
endfunction()

# maicord_add_tests(<target> [TIMEOUT <seconds>] [COST <n>] [LABELS <label>...]
#                   [RESOURCE_LOCK <lock>...] [ENVIRONMENT <VAR=value>...]
#                   [WORKING_DIRECTORY <dir>] [PASS_REGULAR_EXPRESSION <regex>]
#                   [ENV <VAR=value>...] TEST <name> [<argument>...] [TEST ...]...)
# One CTest case per TEST row, running <target> with the row's arguments and
# the shared properties. ENV runs the target through `cmake -E env`. The test
# manifest (maicord_write_test_manifest) records <target> for each case.
function(maicord_add_tests target)
    list(FIND ARGN TEST first)
    if(first LESS 0)
        message(FATAL_ERROR "maicord_add_tests(${target}) has no TEST row")
    endif()
    list(SUBLIST ARGN 0 ${first} options)
    list(SUBLIST ARGN ${first} -1 rows)
    cmake_parse_arguments(ARG "" "TIMEOUT;COST;WORKING_DIRECTORY;PASS_REGULAR_EXPRESSION"
        "LABELS;RESOURCE_LOCK;ENVIRONMENT;ENV" ${options})
    get_property(bundle GLOBAL PROPERTY MAICORD_BUNDLE_OF_${target})
    if(bundle)
        get_property(entry GLOBAL PROPERTY MAICORD_BUNDLE_ENTRY_${target})
        list(APPEND ARG_ENVIRONMENT MAICORD_TEST_ENTRY=${entry})
        set(target ${bundle})
    endif()
    set(command ${target})
    if(ARG_ENV)
        set(command "${CMAKE_COMMAND}" -E env ${ARG_ENV} "$<TARGET_FILE:${target}>")
    endif()
    set(name "")
    foreach(word IN LISTS rows ITEMS TEST)
        if(word STREQUAL "TEST")
            if(NOT name STREQUAL "")
                add_test(NAME ${name} COMMAND ${command} ${arguments})
                set_property(GLOBAL PROPERTY MAICORD_TEST_TARGET_${name} ${target})
                foreach(property IN ITEMS TIMEOUT COST WORKING_DIRECTORY PASS_REGULAR_EXPRESSION LABELS
                        RESOURCE_LOCK ENVIRONMENT)
                    if(DEFINED ARG_${property})
                        set_property(TEST ${name} PROPERTY ${property} ${ARG_${property}})
                    endif()
                endforeach()
            endif()
            set(name "")
            set(arguments)
        elseif(name STREQUAL "")
            set(name "${word}")
        else()
            list(APPEND arguments "${word}")
        endif()
    endforeach()
endfunction()

# maicord_test_bundle_member(<bundle> <target> ENTRY <symbol> SOURCES <file>...
#     [LIBS <library>...] [INCLUDES <dir>...])
# Adds a test to the executable <bundle>, which maicord_finish_test_bundles()
# creates once the registry is complete. The bundle runs the member whose
# entry symbol MAICORD_TEST_ENTRY names, which maicord_add_tests(<target>)
# sets for each case. Members must not define symbols the others or the
# libraries define, and need no DEFINES, OPTIONS or DEPENDS of their own.
function(maicord_test_bundle_member bundle target)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "ENTRY" "SOURCES;LIBS;INCLUDES")
    if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_ENTRY OR NOT ARG_SOURCES)
        message(FATAL_ERROR "maicord_test_bundle_member(${bundle} ${target}): needs ENTRY and SOURCES; "
            "unexpected: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    set_property(GLOBAL APPEND PROPERTY MAICORD_TEST_BUNDLES ${bundle})
    set_property(GLOBAL PROPERTY MAICORD_BUNDLE_OF_${target} ${bundle})
    set_property(GLOBAL PROPERTY MAICORD_BUNDLE_ENTRY_${target} ${ARG_ENTRY})
    set_property(GLOBAL APPEND PROPERTY MAICORD_BUNDLE_ENTRIES_${bundle} ${ARG_ENTRY})
    set_property(GLOBAL APPEND PROPERTY MAICORD_BUNDLE_SOURCES_${bundle} ${ARG_SOURCES})
    if(ARG_LIBS)
        set_property(GLOBAL APPEND PROPERTY MAICORD_BUNDLE_LIBS_${bundle} ${ARG_LIBS})
    endif()
    if(ARG_INCLUDES)
        set_property(GLOBAL APPEND PROPERTY MAICORD_BUNDLE_INCLUDES_${bundle} ${ARG_INCLUDES})
    endif()
endfunction()

# maicord_finish_test_bundles(): creates every bundle executable with a
# generated /ENTRY that calls the member selected by MAICORD_TEST_ENTRY.
function(maicord_finish_test_bundles)
    get_property(bundles GLOBAL PROPERTY MAICORD_TEST_BUNDLES)
    list(REMOVE_DUPLICATES bundles)
    string(ASCII 10 newline)
    foreach(bundle IN LISTS bundles)
        foreach(kind IN ITEMS ENTRIES SOURCES LIBS INCLUDES)
            get_property(${kind} GLOBAL PROPERTY MAICORD_BUNDLE_${kind}_${bundle})
            list(REMOVE_DUPLICATES ${kind})
        endforeach()
        set(declarations)
        set(table)
        foreach(entry IN LISTS ENTRIES)
            string(APPEND declarations "unsigned ${entry}();${newline}")
            string(APPEND table "    {L\"${entry}\", ${entry}},${newline}")
        endforeach()
        set(dispatcher "${CMAKE_CURRENT_BINARY_DIR}/test_bundles/${bundle}.cpp")
        file(CONFIGURE OUTPUT "${dispatcher}" CONTENT [=[
#include "maicord/core/win32_kernel.hpp"

extern "C" {
@declarations@}

namespace {
struct Entry {
    const wchar_t* name;
    unsigned (*run)();
};
constexpr Entry entries[]{
@table@};
} // namespace

// Runs the member test whose entry symbol MAICORD_TEST_ENTRY names; any other
// value lists the members.
extern "C" __declspec(noreturn) void WINAPI maicord_test_bundle_entry() noexcept {
    wchar_t name[128];
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_TEST_ENTRY", name, 128);
    if (length != 0 && length < 128) {
        for (const Entry& entry : entries) {
            const wchar_t* a = entry.name;
            const wchar_t* b = name;
            while (*a && *a == *b) {
                ++a;
                ++b;
            }
            if (*a == *b)
                ExitProcess(entry.run());
        }
    }
    for (const Entry& entry : entries) {
        char line[160];
        unsigned used = 0;
        for (; entry.name[used] && used < 150; ++used)
            line[used] = static_cast<char>(entry.name[used]);
        line[used++] = '\n';
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, used, &written, nullptr);
    }
    ExitProcess(2);
}
]=] @ONLY)
        set(arguments ENTRY maicord_test_bundle_entry SOURCES "${dispatcher}" ${SOURCES})
        if(LIBS)
            list(APPEND arguments LIBS ${LIBS})
        endif()
        if(INCLUDES)
            list(APPEND arguments INCLUDES ${INCLUDES})
        endif()
        maicord_program(${bundle} ${arguments})
    endforeach()
endfunction()

# maicord_font_test(<target> <entry> <source> [UNICODE] [<maicord_test argument>...])
# A tests/<source>.cpp executable linking maicord_core, maicord_font (then
# maicord_unicode with UNICODE) and kernel32.
function(maicord_font_test target entry source)
    set(libs maicord_core maicord_font)
    if(ARGC GREATER 3 AND ARGV3 STREQUAL "UNICODE")
        list(APPEND libs maicord_unicode)
        list(REMOVE_AT ARGN 0)
    endif()
    maicord_test(${target} ENTRY ${entry} SOURCES tests/${source}.cpp LIBS ${libs} kernel32 ${ARGN})
endfunction()

# maicord_native_profile_tests(<timeout> <profile> <family>)
# MaiCordNativeAppLineTests --<profile>-profile (an -app suffix is dropped
# from the option) under <family>, Segoe UI and the bundled BioRhyme, as
# maicord_native_<profile>[_default|_private], labelled native-text-<profile>.
function(maicord_native_profile_tests timeout profile family)
    string(REPLACE "-" "_" test "maicord_native_${profile}")
    string(REGEX REPLACE "-app-profile$" "-profile" option "--${profile}-profile")
    maicord_add_tests(MaiCordNativeAppLineTests TIMEOUT ${timeout} LABELS native-text-${profile}
        TEST ${test} ${option} --family "${family}" 400
        TEST ${test}_default ${option} --family "Segoe UI" 400
        TEST ${test}_private ${option} "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf")
endfunction()

# maicord_indic_plan_tests(<script> <mode>...)
# MaiCordDevanagariPlanTests --<script>-<mode> as maicord_native_<script>_<mode>,
# labelled native-text-<script>-plan.
function(maicord_indic_plan_tests script)
    set(rows)
    foreach(mode IN LISTS ARGN)
        string(REPLACE "_" "-" option "--${script}-${mode}")
        list(APPEND rows TEST maicord_native_${script}_${mode} ${option})
    endforeach()
    maicord_add_tests(MaiCordDevanagariPlanTests TIMEOUT 5 LABELS native-text-${script}-plan ${rows})
endfunction()

# maicord_ui_probe_test(<test> <variable> [BOOTSTRAP] [<maicord_add_tests property>...])
# MaiCordUiProbe with MAICORD_<variable>=1, the native bootstrap when
# BOOTSTRAP is given, and GPU metrics.
function(maicord_ui_probe_test test variable)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "BOOTSTRAP" "" "")
    set(environment MAICORD_${variable}=1)
    if(ARG_BOOTSTRAP)
        list(APPEND environment MAICORD_NATIVE_BOOTSTRAP_PROBE=1)
    endif()
    maicord_add_tests(MaiCordUiProbe ${ARG_UNPARSED_ARGUMENTS} ENV ${environment} MAICORD_GPU_METRICS=1
        TEST ${test})
endfunction()

# maicord_c_plugin(<target> <entry> <source>...)
# A plain C plugin DLL against the public SDK header, freestanding like the
# client: no CRT, no stack cookies, runtime helpers from maicord_runtime.
function(maicord_c_plugin target entry)
    add_library(${target} SHARED ${ARGN} $<TARGET_OBJECTS:maicord_runtime>)
    target_link_libraries(${target} PRIVATE maicord_compile_contract kernel32)
    target_compile_options(${target} PRIVATE /Zl /GS- /W4 /WX /utf-8 /O2 /Ob2 ${MAICORD_WHOLE_PROGRAM_OPTION})
    target_link_options(${target} PRIVATE /ENTRY:${entry} /SUBSYSTEM:WINDOWS,6.02)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

# Tests that exercise the shipping pair itself (update staging, the account
# DLL's exports) depend on it, unless the caller supplies that pair: CI hands
# the client job's binaries to the test job, and a local run can keep using a
# client that is open (tools/build.ps1 -PrebuiltClient).
function(maicord_test_uses_client target)
    set_property(TARGET ${target} PROPERTY MAICORD_TEST_USES_CLIENT ON)
    if(NOT MAICORD_TEST_PREBUILT_CLIENT)
        add_dependencies(${target} ${ARGN})
    endif()
endfunction()

# Temporary tests do not ship, so they skip whole-program optimization. A /GL
# object defers all code generation to the final link, so each of the ~200 test
# executables re-generated code for every library function it pulled in; those
# LTCG links were about two thirds of the CPU time of a -Test build. Tests link
# non-/GL twins of the production static libraries instead (same sources,
# definitions, include paths, per-source options and /O level; only /GL and
# /LTCG differ) and compile their own sources without /GL. Called once, after
# every test target exists; appends the twins to the temporary target list.
function(maicord_add_test_codegen_twins target_list_variable)
    set(libraries)
    foreach(library IN LISTS maicord_production_targets ITEMS maicord_opus_decoder maicord_opus_encoder)
        get_target_property(type ${library} TYPE)
        if(type STREQUAL "STATIC_LIBRARY")
            list(APPEND libraries ${library})
        endif()
    endforeach()
    set(twins)
    foreach(library IN LISTS libraries)
        set(twin ${library}_test)
        get_target_property(source_directory ${library} SOURCE_DIR)
        get_target_property(sources ${library} SOURCES)
        set(twin_sources)
        foreach(source IN LISTS sources)
            # Sources from another directory keep that directory's per-source options.
            if(NOT source_directory STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT source MATCHES "^\\$<")
                cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${source_directory}")
                get_source_file_property(source_options "${source}" DIRECTORY "${source_directory}" COMPILE_OPTIONS)
                if(source_options)
                    set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "${source_options}")
                endif()
            endif()
            list(APPEND twin_sources "${source}")
        endforeach()
        add_library(${twin} STATIC ${twin_sources})
        foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES
                LINK_LIBRARIES INTERFACE_LINK_LIBRARIES INTERFACE_INCLUDE_DIRECTORIES
                INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_FEATURES
                INTERFACE_LINK_OPTIONS STATIC_LIBRARY_OPTIONS C_STANDARD C_EXTENSIONS CXX_STANDARD
                CXX_EXTENSIONS MSVC_RUNTIME_LIBRARY)
            get_target_property(value ${library} ${property})
            if(NOT value STREQUAL "value-NOTFOUND")
                if(property STREQUAL "COMPILE_OPTIONS")
                    list(REMOVE_ITEM value ${MAICORD_WHOLE_PROGRAM_OPTION})
                endif()
                set_property(TARGET ${twin} PROPERTY ${property} "${value}")
            endif()
        endforeach()
        get_target_property(dependencies ${library} MANUALLY_ADDED_DEPENDENCIES)
        if(NOT dependencies)
            set(dependencies)
        endif()
        foreach(dependency IN LISTS dependencies)
            if(dependency IN_LIST libraries)
                set(dependency ${dependency}_test)
            endif()
            add_dependencies(${twin} ${dependency})
        endforeach()
        list(APPEND twins ${twin})
    endforeach()
    foreach(target IN LISTS ${target_list_variable} twins)
        get_target_property(type ${target} TYPE)
        if(NOT type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY|STATIC_LIBRARY|OBJECT_LIBRARY)$")
            continue()
        endif()
        set_property(TARGET ${target} PROPERTY MAICORD_TEST_CODEGEN ON)
        foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(items ${target} ${property})
            if(items STREQUAL "items-NOTFOUND")
                continue()
            endif()
            set(mapped)
            foreach(item IN LISTS items)
                if(item IN_LIST libraries)
                    set(item ${item}_test)
                elseif(item MATCHES "^\\$<LINK_ONLY:([A-Za-z0-9_]+)>$" AND CMAKE_MATCH_1 IN_LIST libraries)
                    set(item "$<LINK_ONLY:${CMAKE_MATCH_1}_test>")
                endif()
                list(APPEND mapped "${item}")
            endforeach()
            set_property(TARGET ${target} PROPERTY ${property} "${mapped}")
        endforeach()
    endforeach()
    set(${target_list_variable} ${${target_list_variable}} ${twins} PARENT_SCOPE)
endfunction()
# Test executables and their non-/GL archives often compile the same source with identical settings
# (application_state.cpp in about 30, log.cpp in nearly 40). Each such source
# is compiled once into an OBJECT library shared by the executables whose
# settings match exactly: same own include directories, definitions, options,
# features, language settings and runtime, and dependencies whose only usage
# requirements come from the compile contract. The shared object is the one
# each consumer would have built itself. Shared/module fixtures keep their
# implicit export definitions and position-independent compilation separate.
# Called after the codegen twins.
function(maicord_share_test_objects target_list_variable)
    set(properties INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS COMPILE_FEATURES
        PRECOMPILE_HEADERS CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS MSVC_RUNTIME_LIBRARY
        POSITION_INDEPENDENT_CODE CXX_VISIBILITY_PRESET VISIBILITY_INLINES_HIDDEN)
    set(signatures)
    set(compilations 0)
    foreach(target IN LISTS ${target_list_variable})
        get_target_property(type ${target} TYPE)
        get_target_property(codegen ${target} MAICORD_TEST_CODEGEN)
        get_target_property(links ${target} LINK_LIBRARIES)
        if(NOT type MATCHES "^(EXECUTABLE|STATIC_LIBRARY)$" OR NOT codegen
                OR NOT "maicord_compile_contract" IN_LIST links)
            continue()
        endif()
        # Every usage requirement must come from the compile contract.
        set(pending ${links})
        set(visited maicord_compile_contract)
        set(plain TRUE)
        while(pending AND plain)
            list(POP_FRONT pending link)
            if(link MATCHES "^\\$<LINK_ONLY:" OR link IN_LIST visited OR (NOT link MATCHES "^\\$<" AND NOT TARGET ${link}))
                continue()
            endif()
            if(link MATCHES "^\\$<")
                set(plain FALSE)
                break()
            endif()
            list(APPEND visited ${link})
            foreach(property IN ITEMS INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS
                    INTERFACE_INCLUDE_DIRECTORIES INTERFACE_COMPILE_FEATURES INTERFACE_PRECOMPILE_HEADERS
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
                get_target_property(value ${link} ${property})
                if(value)
                    set(plain FALSE)
                endif()
            endforeach()
            get_target_property(interface ${link} INTERFACE_LINK_LIBRARIES)
            if(interface)
                list(APPEND pending ${interface})
            endif()
        endwhile()
        if(NOT plain)
            continue()
        endif()
        set(signature)
        foreach(property IN LISTS properties)
            get_target_property(value ${target} ${property})
            string(APPEND signature "${property}=${value}|")
        endforeach()
        string(SHA1 signature "${signature}")
        set(signature_${target} ${signature})
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS sources)
            if(source MATCHES "^\\$<" OR NOT source MATCHES "\\.cpp$")
                continue()
            endif()
            set(path "${source}")
            cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
            cmake_path(IS_PREFIX CMAKE_CURRENT_BINARY_DIR "${path}" generated)
            if(generated OR NOT EXISTS "${path}")
                continue()
            endif()
            math(EXPR compilations "${compilations} + 1")
            string(SHA1 key "${path}|${signature}")
            if(NOT DEFINED users_${key})
                list(APPEND signatures ${key})
                set(path_${key} "${path}")
            endif()
            list(APPEND users_${key} ${target})
        endforeach()
    endforeach()
    set(shared)
    set(rewritten)
    set(index 0)
    set(saved 0)
    set(archive_saved 0)
    foreach(key IN LISTS signatures)
        list(LENGTH users_${key} count)
        if(count LESS 2)
            continue()
        endif()
        math(EXPR index "${index} + 1")
        math(EXPR saved "${saved} + ${count} - 1")
        set(archives 0)
        foreach(target IN LISTS users_${key})
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "STATIC_LIBRARY")
                math(EXPR archives "${archives} + 1")
            endif()
        endforeach()
        # Without archive sharing, executable users already compiled once and
        # each archive separately. Count the additional compilations removed.
        if(archives EQUAL count)
            math(EXPR archives "${archives} - 1")
        endif()
        math(EXPR archive_saved "${archive_saved} + ${archives}")
        set(library maicord_test_shared_${index})
        list(GET users_${key} 0 first)
        add_library(${library} OBJECT "${path_${key}}")
        foreach(property IN LISTS properties)
            get_target_property(value ${first} ${property})
            if(NOT value STREQUAL "value-NOTFOUND")
                set_property(TARGET ${library} PROPERTY ${property} "${value}")
            endif()
        endforeach()
        # The plain-usage gate proves this contract supplies every compile
        # requirement. Copying a user's link libraries here would create a
        # cycle when one of those archives itself consumes the shared object.
        target_link_libraries(${library} PRIVATE maicord_compile_contract)
        set_property(TARGET ${library} PROPERTY MAICORD_TEST_CODEGEN ON)
        set(dependencies)
        foreach(target IN LISTS users_${key})
            get_target_property(items ${target} MANUALLY_ADDED_DEPENDENCIES)
            if(items)
                list(APPEND dependencies ${items})
            endif()
        endforeach()
        if(dependencies)
            list(REMOVE_DUPLICATES dependencies)
            add_dependencies(${library} ${dependencies})
        endif()
        set(library_${key} ${library})
        list(APPEND rewritten ${users_${key}})
        list(APPEND shared ${library})
    endforeach()
    # Each user's source with a shared object (the key of its path and
    # signature) becomes that object, in place.
    list(REMOVE_DUPLICATES rewritten)
    foreach(target IN LISTS rewritten)
        get_target_property(sources ${target} SOURCES)
        set(replaced)
        foreach(source IN LISTS sources)
            set(path "${source}")
            if(NOT source MATCHES "^\\$<")
                cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
            endif()
            string(SHA1 key "${path}|${signature_${target}}")
            if(DEFINED library_${key})
                list(APPEND replaced "$<TARGET_OBJECTS:${library_${key}}>")
            else()
                list(APPEND replaced "${source}")
            endif()
        endforeach()
        set_property(TARGET ${target} PROPERTY SOURCES "${replaced}")
    endforeach()
    math(EXPR remaining "${compilations} - ${saved}")
    message(STATUS "Compatible test C++ compilations: ${compilations} -> ${remaining} (${saved} duplicates removed; ${archive_saved} from archive sharing)")
    set(${target_list_variable} ${${target_list_variable}} ${shared} PARENT_SCOPE)
endfunction()
# maicord_write_test_manifest(<path> <test>...): one line per test for
# tools/build.ps1: name, CI shard, labels and the executable target it runs,
# tab-separated. Shard "app": the target links the application core or a
# shipping library, or runs the shipping pair itself; "core": the rest, which
# needs neither the client nor the application core.
function(maicord_write_test_manifest path)
    set(manifest)
    foreach(test IN LISTS ARGN)
        get_property(target GLOBAL PROPERTY MAICORD_TEST_TARGET_${test})
        set(shard core)
        if(target)
            if(NOT DEFINED shard_${target})
                set(shard_${target} core)
                get_target_property(client ${target} MAICORD_TEST_USES_CLIENT)
                get_target_property(links ${target} LINK_LIBRARIES)
                if(client)
                    set(shard_${target} app)
                else()
                    foreach(link IN LISTS links)
                        maicord_test_link_is_app("${link}" app)
                        if(app)
                            set(shard_${target} app)
                            break()
                        endif()
                    endforeach()
                endif()
            endif()
            set(shard ${shard_${target}})
        endif()
        get_property(labels TEST ${test} PROPERTY LABELS)
        string(APPEND manifest "${test}\t${shard}\t${labels}\t${target}\n")
    endforeach()
    file(WRITE "${path}" "${manifest}")
endfunction()
# Whether a link item is, or transitively links, a shipping static library or
# the application core's test twin. Memoized per target in global properties.
function(maicord_test_link_is_app link result)
    if(link MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
        set(link "${CMAKE_MATCH_1}")
    endif()
    if(NOT TARGET "${link}")
        set(${result} FALSE PARENT_SCOPE)
        return()
    endif()
    get_property(known GLOBAL PROPERTY MAICORD_TEST_APP_${link} SET)
    if(known)
        get_property(value GLOBAL PROPERTY MAICORD_TEST_APP_${link})
        set(${result} ${value} PARENT_SCOPE)
        return()
    endif()
    set_property(GLOBAL PROPERTY MAICORD_TEST_APP_${link} FALSE)
    get_target_property(type ${link} TYPE)
    set(value FALSE)
    if(link STREQUAL "maicord_app_core_test" OR (type STREQUAL "STATIC_LIBRARY" AND
       (link IN_LIST maicord_production_targets OR link MATCHES "^maicord_opus_(de|en)coder$")))
        set(value TRUE)
    else()
        get_target_property(interface ${link} INTERFACE_LINK_LIBRARIES)
        foreach(child IN LISTS interface)
            maicord_test_link_is_app("${child}" child_app)
            if(child_app)
                set(value TRUE)
                break()
            endif()
        endforeach()
    endif()
    set_property(GLOBAL PROPERTY MAICORD_TEST_APP_${link} ${value})
    set(${result} ${value} PARENT_SCOPE)
endfunction()
