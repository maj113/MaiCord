# Writes a plugin manifest from its template with the DLL's SHA-256.
#   cmake -DTEMPLATE=<manifest.json.in> -DDLL=<plugin.dll> -DOUTPUT=<manifest.json> -P stamp_plugin_manifest.cmake
if(NOT TEMPLATE OR NOT DLL OR NOT OUTPUT)
    message(FATAL_ERROR "stamp_plugin_manifest: TEMPLATE, DLL and OUTPUT are required.")
endif()
file(SHA256 "${DLL}" SHOWCASE_SHA256)
configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY NEWLINE_STYLE LF)
