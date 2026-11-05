#!/usr/bin/env python3
"""Checks that the Linux client names no Windows feature in the text it shows.

Usage: check_linux_ui_text.py MaiCord

Shared code says "Windows" where the Windows build means the operating system; the Linux build picks another
wording at compile time (include/maicord/core/os_text.hpp, the Linux text of src/app/settings/catalog.cpp). This
reads the image's printable text and fails when a run of it names "Windows" as a word, so a string that was added
without a Linux wording is found by the build and not by a user. The settings
catalog is stored deflated: tests/app/settings/linux_catalog_text.cpp checks it.
"""
import re
import sys

RUN = re.compile(rb"[\x20-\x7e\xc2-\xf4\x80-\xbf]{6,}")
FORBIDDEN = re.compile(rb"\bWindows\b")
ALLOWED = [
    re.compile(rb"Windows NT"),  # a user agent the client sends
]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as image:
        data = image.read()
    found = []
    for run in RUN.finditer(data):
        text = run.group(0)
        if FORBIDDEN.search(text) and not any(allowed.search(text) for allowed in ALLOWED):
            found.append(text.decode("utf-8", "replace"))
    for text in found:
        print("names Windows: " + text)
    print("%s: %d text runs name Windows" % (argv[1], len(found)))
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
