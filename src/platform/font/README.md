# System fonts on Linux

`linux/` replaces the GDI `SystemFontLocator` (`include/maicord/platform/font/system_font_locator.hpp`).

- `libfontconfig.so.1` is opened with `dlopen` for a family, weight and slant (`fontconfig.hpp` lists the functions used) and closed
  again after each catalog query. With no libfontconfig an in-house scan reads the directories of `fonts.conf` (`<dir>`/`<include>`,
  `FONTCONFIG_FILE`/`FONTCONFIG_PATH`, the XDG and NixOS roots).
- Defaults come from `default_fonts.hpp`: GTK `settings.ini` and KDE `kdeglobals`, else fontconfig's `sans-serif`; Noto Color Emoji for
  emoji. The portal's interface and monospace fonts reach the text system through `application_platform_desktop_fonts()`
  (`src/app/linux/desktop_fonts.cpp`) before the first text.
- System fallback asks `FcFontSort` for the route's sample scalar first, then the generated range table
  `src/ui/text/generated/linux_fallback_data.inc` (`python tools/font_reference/linux_fallback.py --ucd <UCD-17.0.0.zip>`, CJK variants
  per language; regenerate after a Unicode version change).
- Colour bitmap emoji (CBLC/CBDT, PNG images) are read by `maicord_font_color` on Linux only (`src/text/opentype`; Windows' Segoe UI Emoji
  is COLR) and reach the colour atlas through `colr_paint_upload_pixels`; `tools/font_reference/make_cbdt_fixture.py` regenerates the
  fixture.
- Names and ranges that differ per system sit under `#if defined(_WIN32)` in `src/ui/text/native_text_catalog*.inc` and
  `native_system_fallback*.inc`.

## Line boxes

A line box on Linux is sized by the primary font alone, as under Discord's fixed CSS line height: a span drawn with a system fallback
face or the emoji face takes the ascent and descent of the face the text asked for (`line_metrics_binding` in
`src/ui/text/native_text_styled.inc`, which feeds the paragraph fitter's per-span `SfntFontLineParts`). Noto Sans Symbols (hhea
1480/-570 against Noto Sans 1069/-293) therefore neither lifts the baseline nor grows the line, the header row or the mention pill.
Windows keeps the union of the faces, as DirectWrite computes it and the tests compare. `maicord_native_system_text` checks height,
baseline and pill.

## Tests

`cmake/linux_font_tests.cmake`. The tests hold on any host and are strict about installed families only when
`MAICORD_TEST_FONT_SET=ubuntu` (the registry sets it; the packages are `fonts-dejavu-core`, `fonts-noto-core`, `fonts-noto-cjk`,
`fonts-noto-color-emoji`, `fonts-liberation`). Tests that observe Windows font files (`catalog_test`, `catalog_coverage_test`) are not
registered on Linux. `maicord_font_locator` also runs on NixOS, where libfontconfig is found only through `NIX_LD_LIBRARY_PATH` or the
scan answers.
