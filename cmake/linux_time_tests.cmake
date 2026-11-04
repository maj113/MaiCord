# The time zone and local time tests of the Linux build. The TZif reader is pure and also runs in the Windows suite;
# the local zone tests start the same executable with $TZ and $TZDIR set to the zone files of tests/fixtures/zoneinfo
# (written by tools/tzif_fixtures.py), so they do not depend on the host's tzdata.

maicord_test(MaiCordTzifTests ENTRY maicord_tzif_entry
    SOURCES tests/platform/time/tzif_test.cpp
    LIBS maicord_core INCLUDES src
    TIMEOUT 10 TEST maicord_tzif)

set(maicord_zoneinfo ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/zoneinfo)
maicord_test(MaiCordLocalTimeTests ENTRY maicord_local_time_entry
    SOURCES tests/os/local_time_test.cpp
    LIBS maicord_desktop_system maicord_core
    TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=Europe/Berlin
    TEST maicord_local_time_berlin berlin)
maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=:America/New_York
    TEST maicord_local_time_new_york new_york)
maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZ=${maicord_zoneinfo}/Asia/Kolkata
    TEST maicord_local_time_path kolkata)
maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=Australia/Lord_Howe
    TEST maicord_local_time_lord_howe lord_howe)
maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZ=EST5EDT,M3.2.0,M11.1.0
    TEST maicord_local_time_rule rule)
maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZ=
    TEST maicord_local_time_empty utc)
maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=Nowhere/Land
    TEST maicord_local_time_missing missing)
# Whatever zone the host has ($TZ unset: /etc/localtime).
maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 TEST maicord_local_time_system system)
maicord_add_tests(MaiCordLocalTimeTests TIMEOUT 10 ENV TZDIR=${maicord_zoneinfo} TZ=America/New_York
    TEST maicord_local_time_log log)

# Date and time formatting (English names; the hour cycle comes from LC_ALL, LC_TIME or LANG, in that order).
maicord_test(MaiCordDateFormatTests ENTRY maicord_date_format_entry
    SOURCES tests/os/date_format_test.cpp
    LIBS maicord_core
    TIMEOUT 10 ENV LC_ALL=en_US.UTF-8
    TEST maicord_date_format_en_us 12)
maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL=de_DE.UTF-8 TEST maicord_date_format_de_de 24)
maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL=C TEST maicord_date_format_c 12)
maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL=en_GB.UTF-8 TEST maicord_date_format_en_gb 24)
# LC_TIME beats LANG, and LC_ALL beats both.
maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL= LC_TIME=fr_FR.UTF-8 LANG=en_US.UTF-8
    TEST maicord_date_format_lc_time 24)
maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL=en_US.UTF-8 LC_TIME=de_DE.UTF-8 LANG=de_DE.UTF-8
    TEST maicord_date_format_lc_all 12)
maicord_add_tests(MaiCordDateFormatTests TIMEOUT 10 ENV LC_ALL= LC_TIME= LANG=ja_JP.UTF-8
    TEST maicord_date_format_lang 24)
