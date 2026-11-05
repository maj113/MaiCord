#pragma once

// The signed manifest's fields that name the system's package, as string literals the updater's fixtures share. The
// Windows client reads x64 and MaiCord-x64-p2-<version>.zip; the Linux client linux-x64 and the release tarball of the
// version (manifest.hpp). The fixtures' versions are small numbers, which are development builds 0.0.0-dev.<n>.
#if defined(_WIN32)
#define MAICORD_TEST_ARCH "x64"
#define MAICORD_TEST_ARTIFACT(version) "MaiCord-x64-p2-" #version ".zip"
#else
#define MAICORD_TEST_ARCH "linux-x64"
#define MAICORD_TEST_ARTIFACT(version) "maicord-0.0.0-dev." #version "-linux-x86_64.tar.gz"
#endif

// A complete manifest of one channel, version, size and digest, valid between times 90 and 200.
#define MAICORD_TEST_MANIFEST(channel, version, bytes, sha256)                                                          \
    R"({"format":2,"product":"MaiCord","arch":")" MAICORD_TEST_ARCH R"(","channel":")" channel R"(","version":)" #version \
        R"(,"issued":90,"expires":200,"bytes":)" #bytes R"(,"sha256":")" sha256 R"(","artifact":")"                       \
        MAICORD_TEST_ARTIFACT(version) R"("})"
