# The release identity the client is built with, shared by both systems' shipping targets (cmake/client.cmake): the
# self-updater's publisher, key, installed version and commit, and the plugin catalog's publisher keys. They reach
# src/app/update/entry.cpp and src/app/plugins/catalog_app.cpp as compile definitions.
# Needs ${Python3_EXECUTABLE}.

# Self-updater publisher. Environment variables override the official defaults,
# as the release workflow does; setting both empty compiles the updater disabled.
set(maicord_default_update_repository "maj113/MaiCord")
set(maicord_default_update_public_key
    "046d75f38397386d109c1b090e60fe720bf12e84ee1579e39fca81fd33bc65330129a46073d9555a6d9141ce090e114e7da3389d9c37b9fcedaf3eb9ef26301f44")
if(DEFINED ENV{MAICORD_UPDATE_REPOSITORY})
    set(maicord_default_update_repository "$ENV{MAICORD_UPDATE_REPOSITORY}")
endif()
if(DEFINED ENV{MAICORD_UPDATE_PUBLIC_KEY})
    set(maicord_default_update_public_key "$ENV{MAICORD_UPDATE_PUBLIC_KEY}")
endif()
set(MAICORD_UPDATE_REPOSITORY "${maicord_default_update_repository}" CACHE STRING
    "GitHub owner/repo whose releases publish signed MaiCord updates")
set(MAICORD_UPDATE_PUBLIC_KEY "${maicord_default_update_public_key}" CACHE STRING
    "Update publisher ECDSA P-256 public key: 130 hex digits (uncompressed SEC1 point)")
# The installed version. CI and release builds pass it (MAICORD_UPDATE_VERSION
# and MAICORD_UPDATE_COMMIT). Left empty, it is this checkout's build identity
# (tools/release/maicord_release.py identity), numbered exactly as CI numbers
# master builds: X.Y.Z-dev.N for N commits after the last stable tag, X.Y.Z on
# the tag. A local build is then never older than the published build of its
# commit, so the updater never offers it an older one. tools/build.ps1
# configures on every run, so each build is numbered for the current HEAD.
set(MAICORD_UPDATE_VERSION "$ENV{MAICORD_UPDATE_VERSION}" CACHE STRING
    "Installed version X.Y.Z or X.Y.Z-dev.N (N 1-998); empty for this checkout's git build identity")
set(MAICORD_UPDATE_COMMIT "$ENV{MAICORD_UPDATE_COMMIT}" CACHE STRING
    "Source commit (40 lowercase hex digits) shown with the version; empty for HEAD with a git build identity")
set(maicord_update_version "${MAICORD_UPDATE_VERSION}")
set(maicord_update_commit "${MAICORD_UPDATE_COMMIT}")
set(maicord_update_repository "${MAICORD_UPDATE_REPOSITORY}")
set(maicord_update_public_key "${MAICORD_UPDATE_PUBLIC_KEY}")
if(NOT maicord_update_version)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=GITHUB_OUTPUT
            "${Python3_EXECUTABLE}" -B tools/release/maicord_release.py identity --repository .
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE maicord_identity_result
        OUTPUT_VARIABLE maicord_identity
        ERROR_VARIABLE maicord_identity_error
        ERROR_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "version=([^\r\n]+)" maicord_identity_version "${maicord_identity}")
    set(maicord_identity_version "${CMAKE_MATCH_1}")
    string(REGEX MATCH "commit=([0-9a-f]+)" maicord_identity_commit "${maicord_identity}")
    set(maicord_identity_commit "${CMAKE_MATCH_1}")
    if(maicord_identity_result EQUAL 0 AND maicord_identity_version)
        set(maicord_update_version "${maicord_identity_version}")
        if(NOT maicord_update_commit)
            set(maicord_update_commit "${maicord_identity_commit}")
        endif()
        message(STATUS "MaiCord build identity: ${maicord_update_version} (${maicord_update_commit})")
    else()
        # Without its number a build can't tell whether a release is newer.
        message(WARNING "No git build identity (${maicord_identity_error}). MaiCord builds as the project version "
            "${PROJECT_VERSION} with the updater disabled; set MAICORD_UPDATE_VERSION to number it.")
        set(maicord_update_repository "")
        set(maicord_update_public_key "")
    endif()
endif()
set(maicord_update_dev 0)
if(NOT maicord_update_version)
    set(maicord_update_major ${PROJECT_VERSION_MAJOR})
    set(maicord_update_minor ${PROJECT_VERSION_MINOR})
    set(maicord_update_patch ${PROJECT_VERSION_PATCH})
elseif(maicord_update_version MATCHES "^(0|[1-9][0-9]?[0-9]?)\\.(0|[1-9][0-9]?[0-9]?)\\.(0|[1-9][0-9]?[0-9]?)(-dev\\.([1-9][0-9]?[0-9]?))?$"
        AND NOT CMAKE_MATCH_5 STREQUAL "999")
    set(maicord_update_major ${CMAKE_MATCH_1})
    set(maicord_update_minor ${CMAKE_MATCH_2})
    set(maicord_update_patch ${CMAKE_MATCH_3})
    if(CMAKE_MATCH_5)
        set(maicord_update_dev ${CMAKE_MATCH_5})
    endif()
else()
    message(FATAL_ERROR "MAICORD_UPDATE_VERSION must be empty, X.Y.Z or X.Y.Z-dev.N (components 0-999, N 1-998).")
endif()
string(LENGTH "${maicord_update_commit}" maicord_update_commit_length)
if(maicord_update_commit AND (NOT maicord_update_commit_length EQUAL 40 OR NOT maicord_update_commit MATCHES "^[0-9a-f]+$"))
    message(FATAL_ERROR "MAICORD_UPDATE_COMMIT must be empty or 40 lowercase hex digits.")
endif()
set(maicord_owner_repository_pattern "^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?/[A-Za-z0-9_.-]+$")
# The publisher keys are uncompressed P-256 points: 130 hex digits starting 04.
function(maicord_check_p256_point variable)
    string(LENGTH "${${variable}}" length)
    if(NOT length EQUAL 130 OR NOT ${variable} MATCHES "^04[0-9A-Fa-f]+$")
        message(FATAL_ERROR "${variable} must be 130 hex digits: an uncompressed point (04...).")
    endif()
endfunction()
if(MAICORD_UPDATE_REPOSITORY OR MAICORD_UPDATE_PUBLIC_KEY)
    # The owner is a GitHub login: it also names the Pages host <owner>.github.io.
    if(NOT MAICORD_UPDATE_REPOSITORY MATCHES "${maicord_owner_repository_pattern}")
        message(FATAL_ERROR "MAICORD_UPDATE_REPOSITORY must be owner/repo.")
    endif()
    maicord_check_p256_point(MAICORD_UPDATE_PUBLIC_KEY)
endif()
# Whether the release channels carry the Linux packages, the tarball and the AppImage (maicord_release.py linux [--appimage],
# MAICORD_LINUX_RELEASES in .github/workflows/release.yml). A Linux build compiled without it never enables the updater, tarball install or not
# (src/app/update/entry.cpp). The release workflow's Linux job sets the variable from the same repository variable that
# turns the job on; the Windows client has no use for it.
if(DEFINED ENV{MAICORD_LINUX_RELEASES} AND "$ENV{MAICORD_LINUX_RELEASES}" STREQUAL "true")
    set(maicord_default_linux_updates_published ON)
else()
    set(maicord_default_linux_updates_published OFF)
endif()
option(MAICORD_LINUX_UPDATES_PUBLISHED "The release channels carry the Linux packages: a tarball install or an AppImage may update itself"
    "${maicord_default_linux_updates_published}")
set(maicord_update_definitions
    "MAICORD_UPDATE_REPOSITORY=\"${maicord_update_repository}\";MAICORD_UPDATE_PUBLIC_KEY=\"${maicord_update_public_key}\";MAICORD_UPDATE_DEV=${maicord_update_dev};MAICORD_UPDATE_COMMIT=\"${maicord_update_commit}\";MAICORD_VERSION_MAJOR=${maicord_update_major};MAICORD_VERSION_MINOR=${maicord_update_minor};MAICORD_VERSION_PATCH=${maicord_update_patch}")
if(NOT WIN32)
    if(MAICORD_LINUX_UPDATES_PUBLISHED)
        list(APPEND maicord_update_definitions "MAICORD_LINUX_UPDATES_PUBLISHED=1")
    else()
        list(APPEND maicord_update_definitions "MAICORD_LINUX_UPDATES_PUBLISHED=0")
    endif()
endif()
set_source_files_properties(src/app/update/entry.cpp PROPERTIES COMPILE_DEFINITIONS "${maicord_update_definitions}")

# The official catalog (maj113/MaiCord-plugins on GitHub Pages): the current
# and the next (offline) P-256 key. Setting both keys empty compiles the
# catalog as "not configured": installed items keep working and nothing is
# fetched. Environment variables override the defaults, as for the updater.
set(maicord_default_plugin_catalog_repository "maj113/MaiCord-plugins")
set(maicord_default_plugin_catalog_public_key
    "04997fc924ff9cf99c686663f870d003a5898559a319abffa57dd9816b62b94ed487c2cc013bd3ed30c2486a6fd4c5e38a2a30245079ec249b208cef276edc874a")
set(maicord_default_plugin_catalog_next_public_key
    "0482c655b56792ca3193d742aa145e6f25093f771c0dc003ecaa763217ed25b2ecf22b1308d44e7b4dfe6b2fdd4e6831f589bfb9adda811e47c4e30b5c3a5885da")
foreach(name REPOSITORY PUBLIC_KEY NEXT_PUBLIC_KEY)
    string(TOLOWER "${name}" lower)
    if(DEFINED ENV{MAICORD_PLUGIN_CATALOG_${name}})
        set(maicord_default_plugin_catalog_${lower} "$ENV{MAICORD_PLUGIN_CATALOG_${name}}")
    endif()
endforeach()
set(MAICORD_PLUGIN_CATALOG_REPOSITORY "${maicord_default_plugin_catalog_repository}" CACHE STRING
    "GitHub owner/repo whose Pages site publishes the signed official plugin catalog")
set(MAICORD_PLUGIN_CATALOG_PUBLIC_KEY "${maicord_default_plugin_catalog_public_key}" CACHE STRING
    "Plugin catalog ECDSA P-256 public key (130 hex digits); empty: the official catalog is not configured")
set(MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY "${maicord_default_plugin_catalog_next_public_key}" CACHE STRING
    "The next plugin catalog key, accepted alongside the current one for rotation (130 hex digits or empty)")
if(MAICORD_PLUGIN_CATALOG_PUBLIC_KEY OR MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY)
    if(NOT MAICORD_PLUGIN_CATALOG_REPOSITORY MATCHES "${maicord_owner_repository_pattern}")
        message(FATAL_ERROR "MAICORD_PLUGIN_CATALOG_REPOSITORY must be owner/repo.")
    endif()
    if(NOT MAICORD_PLUGIN_CATALOG_PUBLIC_KEY)
        message(FATAL_ERROR "MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY needs MAICORD_PLUGIN_CATALOG_PUBLIC_KEY.")
    endif()
    foreach(key MAICORD_PLUGIN_CATALOG_PUBLIC_KEY MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY)
        if(${key})
            maicord_check_p256_point(${key})
        endif()
    endforeach()
endif()
set_source_files_properties(src/app/plugins/catalog_app.cpp PROPERTIES COMPILE_DEFINITIONS
    "MAICORD_PLUGIN_CATALOG_REPOSITORY=\"${MAICORD_PLUGIN_CATALOG_REPOSITORY}\";MAICORD_PLUGIN_CATALOG_PUBLIC_KEY=\"${MAICORD_PLUGIN_CATALOG_PUBLIC_KEY}\";MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY=\"${MAICORD_PLUGIN_CATALOG_NEXT_PUBLIC_KEY}\"")
