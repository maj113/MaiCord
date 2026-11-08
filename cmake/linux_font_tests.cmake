# The tests of the Linux text platform: the system font locator over fontconfig and over the directory scan, and what
# builds on it. Included from cmake/linux_test_registry.cmake. They read the host's installed fonts; the cases that
# need the Ubuntu font packages (fonts-dejavu-core, fonts-noto-core, fonts-noto-cjk, fonts-noto-color-emoji) run when
# MAICORD_TEST_FONT_SET=ubuntu says so, which the registry sets (tools/build_linux.sh's host notes name the packages);
# any other system runs the same executable with the variable unset.
set(maicord_font_set_environment MAICORD_TEST_FONT_SET=ubuntu)

maicord_test(MaiCordFontLocatorTests ENTRY maicord_font_locator_entry
    SOURCES tests/platform/font/system_font_locator.cpp
    LIBS maicord_font_system maicord_font maicord_core
    TIMEOUT 10 ENV ${maicord_font_set_environment}
    TEST maicord_font_locator)
maicord_add_tests(MaiCordFontLocatorTests TIMEOUT 10 ENV ${maicord_font_set_environment}
    TEST maicord_font_locator_scan --scan)
maicord_add_tests(MaiCordFontLocatorTests TIMEOUT 10
    ENV FONTCONFIG_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts/fonts.conf
    TEST maicord_font_locator_fonts_conf --fonts-conf)

# CBLC/CBDT colour bitmaps: the generated fixture and the installed Noto Color Emoji (strict with the Ubuntu set).
maicord_test(MaiCordCbdtTests ENTRY maicord_cbdt_entry SOURCES tests/text/opentype/cbdt_test.cpp
    LIBS maicord_font_color maicord_font_system maicord_font maicord_media_codec maicord_compression maicord_core
    TIMEOUT 15 ENV ${maicord_font_set_environment}
    TEST maicord_native_cbdt)
maicord_add_tests(MaiCordCbdtTests TIMEOUT 5 TEST maicord_native_cbdt_fixture --fixture)

# The text system over the installed fonts: shaping smoke checks (Latin, CJK, Devanagari, Arabic, Thai, Hebrew, emoji)
# and the language-specific CJK fallback order, and a bold chat paragraph whose emoji fall back to the bitmap colour
# face (refused, not crashed on).
maicord_test(MaiCordSystemTextTests ENTRY maicord_system_text_entry SOURCES tests/ui/text/system_text_linux_test.cpp
    LIBS maicord_text_system maicord_text_catalog maicord_font_system maicord_font maicord_unicode maicord_core
    TIMEOUT 15 ENV ${maicord_font_set_environment}
    TEST maicord_native_system_text)

# The native text catalog: family lookup, the fallback routes and the generated Linux table.
maicord_test(MaiCordTextCatalogTests ENTRY maicord_text_catalog_entry SOURCES tests/ui/text/native_catalog_linux_test.cpp
    LIBS maicord_text_catalog maicord_font_system maicord_font_color maicord_font maicord_media_codec maicord_compression
        maicord_unicode maicord_core
    TIMEOUT 15 ENV ${maicord_font_set_environment}
    TEST maicord_native_text_catalog)
# The language picks the CJK order (fontconfig's sort for the language, the generated table's variants without it).
foreach(language IN ITEMS "ja_JP.UTF-8|Noto Sans CJK JP" "ko_KR.UTF-8|Noto Sans CJK KR" "zh_CN.UTF-8|Noto Sans CJK SC"
        "zh_TW.UTF-8|Noto Sans CJK TC")
    string(REPLACE "|" ";" language "${language}")
    list(GET language 0 locale)
    list(GET language 1 family)
    string(REGEX REPLACE "_.*" "" short "${locale}")
    string(REGEX REPLACE "^[a-z]+_([A-Z]+).*" "\\1" territory "${locale}")
    string(TOLOWER "${short}_${territory}" name)
    maicord_add_tests(MaiCordSystemTextTests TIMEOUT 15 ENV ${maicord_font_set_environment} LC_ALL=${locale}
        TEST maicord_native_system_text_${name} --language "${family}")
endforeach()
maicord_add_tests(MaiCordTextCatalogTests TIMEOUT 15 ENV ${maicord_font_set_environment}
    TEST maicord_native_text_catalog_scan --scan)

# The font tests of the Windows suite that read the installed fonts, a font file of the repository or temporary files
# and so waited for the files seam and the Linux locator. The ones whose cases compare against Windows' own font
# observations run their embedded and bundled-font cases only (their *_owned_* cases need Segoe UI, Nirmala and the
# like and are not registered).
maicord_font_test(MaiCordFontSessionTests maicord_font_session_entry text/fonts/session_test TIMEOUT 5
    TEST maicord_native_font_session)
maicord_font_test(MaiCordFontInstanceTests maicord_font_store_entry text/fonts/store_test TIMEOUT 5
    LABELS native-text-font-instance TEST maicord_native_font_instances)
maicord_font_test(MaiCordRunPlanTests maicord_run_plan_entry text/shaping/run_plan_test TEST maicord_native_run_plans)
maicord_add_tests(MaiCordRunPlanTests TIMEOUT 5
    TEST maicord_native_run_plan_lazy_units "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf" 400 lazy-units
    TEST maicord_native_run_plan_profiles "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/BioRhyme.ttf" 400 profiles)
maicord_font_test(MaiCordSimulationPlanTests maicord_simulation_plan_entry text/shaping/simulation_plan_test
    TEST maicord_native_simulation_plans "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Orbitron.ttf")
maicord_font_test(MaiCordLatinRunTests maicord_latin_run_entry text/scripts/latin_run_test
    TEST maicord_native_latin_guards)
# The HarfBuzz cluster-start corpora; the owned system-font modes need the Windows fonts.
maicord_font_test(MaiCordSyllabicShapeTests maicord_syllabic_shape_entry text/scripts/syllabic_shape_test UNICODE TIMEOUT 5
    LABELS native-text-syllabic TEST maicord_native_syllabic_shape)
maicord_font_test(MaiCordEmojiRunTests maicord_emoji_run_entry text/scripts/emoji_run_test TIMEOUT 5
    LABELS native-text-emoji-run TEST maicord_native_emoji_runs)
maicord_font_test(MaiCordEmojiLineTests maicord_emoji_line_entry text/scripts/emoji_line_test TEST maicord_native_emoji_lines)
maicord_font_test(MaiCordEmojiParagraphTests maicord_emoji_paragraph_entry text/scripts/emoji_paragraph_test
    TEST maicord_native_emoji_paragraphs)
maicord_test(MaiCordArabicPlanTests ENTRY maicord_arabic_plan_tests_entry SOURCES tests/text/scripts/arabic_plan_test.cpp
    LIBS maicord_core maicord_unicode maicord_font
    BUNDLE MaiCordTextUnitTests TEST maicord_native_arabic_plan)
maicord_add_tests(MaiCordArabicPlanTests TIMEOUT 5 LABELS native-text-arabic-run TEST maicord_native_arabic_run --run)

# A colour bitmap glyph (the CBDT fixture) uploaded into the colour atlas and read back from the GPU.
maicord_test(MaiCordGpuBitmapGlyphTests ENTRY maicord_gpu_bitmap_glyph_entry
    SOURCES tests/gpu/bitmap_glyph.cpp
    LIBS maicord_render_colr_paint maicord_font_color maicord_font maicord_gpu_gles_shaders maicord_gpu_gles
         maicord_compression maicord_core INCLUDES src
    TIMEOUT 20 TEST maicord_gpu_bitmap_glyph)
