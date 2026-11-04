# The spell checker tests of the Linux build. The "fixture" cases use the dictionary of tests/fixtures/hunspell through
# $DICPATH; the language cases need the Ubuntu packages hunspell-en-us and hunspell-de-de (MAICORD_TEST_SPELL_SET=ubuntu,
# which the registry sets) and are skipped without them elsewhere; the system test runs on whatever the host offers with
# each backend.
set(maicord_spell_set_environment MAICORD_TEST_SPELL_SET=ubuntu)
set(maicord_hunspell_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hunspell)

maicord_test(MaiCordSpellcheckLinuxTests ENTRY maicord_spellcheck_linux_entry
    SOURCES tests/platform/desktop/spellcheck_linux_test.cpp
    LIBS maicord_desktop_system maicord_core
    TIMEOUT 10 ENV DICPATH=${maicord_hunspell_fixtures} LANG=xx_XX.UTF-8 LC_ALL= LC_MESSAGES=
    TEST maicord_spellcheck_fixture fixture)
maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10
    ENV DICPATH=${maicord_hunspell_fixtures} LANG=en_US.UTF-8 LC_ALL=xx_XX.UTF-8 LC_MESSAGES=
    TEST maicord_spellcheck_lc_all fixture)
maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10
    ENV DICPATH=${maicord_hunspell_fixtures} LANG=en_US.UTF-8 LC_ALL= LC_MESSAGES=xx_XX
    TEST maicord_spellcheck_lc_messages fixture)
maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10
    ENV MAICORD_HUNSPELL_LIBRARY=libmaicord-no-hunspell.so.0 MAICORD_ENCHANT_LIBRARY=libmaicord-no-enchant.so.2
    TEST maicord_spellcheck_missing missing)
maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=de_DE.UTF-8
    LC_ALL= LC_MESSAGES=
    TEST maicord_spellcheck_de_de language de-DE)
# Austrian German has no dictionary of its own here: the base language's (de_DE) is used.
maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=de_AT.UTF-8
    LC_ALL= LC_MESSAGES=
    TEST maicord_spellcheck_de_at language de)
maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=C LC_ALL=
    LC_MESSAGES=
    TEST maicord_spellcheck_c language en-US)
maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=tlh_XX.UTF-8
    LC_ALL= LC_MESSAGES=
    TEST maicord_spellcheck_unknown_language language en-US)
maicord_add_tests(MaiCordSpellcheckLinuxTests TIMEOUT 10 ENV ${maicord_spell_set_environment} LANG=de_DE.UTF-8
    LC_ALL=en_US.UTF-8 LC_MESSAGES=
    TEST maicord_spellcheck_en_us language en-US)

# The system checker (the test the Windows suite runs against ISpellChecker) with each backend.
maicord_test(MaiCordSystemSpellcheckTests ENTRY maicord_system_spellcheck_entry
    SOURCES tests/platform/desktop/system_spellcheck.cpp
    LIBS maicord_desktop_system maicord_core
    TIMEOUT 10 LABELS system_spellcheck ENV LANG=en_US.UTF-8 LC_ALL= LC_MESSAGES=
    TEST maicord_system_spellcheck)
maicord_add_tests(MaiCordSystemSpellcheckTests TIMEOUT 10 LABELS system_spellcheck
    ENV MAICORD_SPELLCHECK_BACKEND=enchant LANG=en_US.UTF-8 LC_ALL= LC_MESSAGES=
    TEST maicord_system_spellcheck_enchant)
