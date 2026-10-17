#include "maicord/ui/composer_attachment_strip.hpp"

#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {
namespace {

bool extension_in(Utf8View extension, const char* const* list, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        u32 bytes = 0;
        while (list[i][bytes])
            ++bytes;
        if (utf8_equal(extension, Utf8View{list[i], bytes}))
            return true;
    }
    return false;
}

template<u32 N> bool extension_in(Utf8View extension, const char* const (&list)[N]) noexcept {
    return extension_in(extension, list, N);
}

u32 append(Slice<char> output, u32 used, const char* text) noexcept {
    for (u32 i = 0; text[i] && used < output.count; ++i)
        output[used++] = text[i];
    return used;
}

u32 append_unsigned(Slice<char> output, u32 used, u64 value, u32 minimum_digits = 1) noexcept {
    char digits[20];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value && count < 20);
    while (count < minimum_digits && count < 20)
        digits[count++] = '0';
    while (count && used < output.count)
        output[used++] = digits[--count];
    return used;
}

bool clip_push(PaintStream* paint, UiRect clip) noexcept {
    return paint_stream_push_clip(paint, clip.left, clip.top, clip.right, clip.bottom);
}

UiRect centered(UiRect area, float extent) noexcept {
    extent = minimum(extent, minimum(ui_rect_width(area), ui_rect_height(area)));
    const float x = (area.left + area.right - extent) * .5f;
    const float y = (area.top + area.bottom - extent) * .5f;
    return {x, y, x + extent, y + extent};
}

// Spoilered thumbnails are still drawn; the renderer blurs them.
bool shows_thumbnail(const UiComposerAttachmentTile& tile) noexcept {
    return tile.preview_ready && media_asset_key_valid(tile.preview) &&
           tile.phase != UiComposerAttachmentPhase::preparing;
}

UiIcon placeholder_icon(const UiComposerAttachmentTile& tile) noexcept {
    if (tile.phase == UiComposerAttachmentPhase::preparing)
        return UiIcon::sending;
    if (tile.spoiler)
        return UiIcon::eye_off;
    return ui_attachment_file_icon(tile.kind);
}

UiIcon button_icon(const UiComposerAttachmentTile& tile, UiComposerAttachmentPart part) noexcept {
    switch (part) {
    case UiComposerAttachmentPart::retry:
        return UiIcon::retry;
    case UiComposerAttachmentPart::spoiler:
        return tile.spoiler ? UiIcon::eye_off : UiIcon::eye;
    case UiComposerAttachmentPart::edit:
        return UiIcon::edit;
    default:
        return UiIcon::trash;
    }
}

// Status replaces the size while transfer state matters.
u32 status_text(const UiComposerAttachmentTile& tile, Slice<char> output) noexcept {
    switch (tile.phase) {
    case UiComposerAttachmentPhase::preparing:
        return 0;
    case UiComposerAttachmentPhase::failed:
        return append(output, 0, "Failed");
    case UiComposerAttachmentPhase::uploading: {
        const u64 percent =
            tile.bytes ? minimum<u64>(100, static_cast<u64>(static_cast<double>(tile.uploaded_bytes) * 100.0 /
                                                            static_cast<double>(tile.bytes)))
                       : 0;
        return append(output, append_unsigned(output, 0, percent), "%");
    }
    default:
        return ui_attachment_format_size(tile.bytes, output);
    }
}

// Pills hold short ASCII labels; size them without a measuring service.
float tag_width(Utf8View text, float height) noexcept {
    return static_cast<float>(text.bytes) * 6.5f + height * .5f;
}

float status_width(u32 bytes) noexcept {
    return bytes ? static_cast<float>(bytes) * 7.f + 6.f : 0.f;
}

} // namespace

u32 ui_attachment_file_extension(Utf8View filename, Slice<char> output) noexcept {
    if (!filename.data || !filename.bytes || !output.data)
        return 0;
    u32 dot = filename.bytes;
    while (dot && filename.data[dot - 1] != '.' && filename.data[dot - 1] != '/' && filename.data[dot - 1] != '\\')
        --dot;
    if (!dot || filename.data[dot - 1] != '.' || dot == filename.bytes || filename.bytes - dot > 8 ||
        filename.bytes - dot > output.count)
        return 0;
    const u32 bytes = filename.bytes - dot;
    for (u32 i = 0; i < bytes; ++i) {
        const char c = filename.data[dot + i];
        if (static_cast<u8>(c) >= 0x80)
            return 0;
        output[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    }
    return bytes;
}

UiAttachmentFileKind ui_attachment_file_kind(Utf8View filename) noexcept {
    char lowered[8];
    const Utf8View extension{lowered, ui_attachment_file_extension(filename, {lowered, 8})};
    if (!extension.bytes)
        return UiAttachmentFileKind::unknown;
    // svg counts as web code, so web code is tested before the image family.
    static constexpr const char* webcode[]{"html", "xhtml", "htm", "xml", "xsd", "css", "styl", "svg"};
    static constexpr const char* image[]{"png", "jpg", "jpeg", "jfif", "gif", "webp", "avif", "bmp",
                                         "heic", "heif", "tif", "tiff", "ico", "apng"};
    static constexpr const char* video[]{"mp4", "m4v", "mov", "webm", "mkv", "avi", "wmv", "ogv", "3gp"};
    static constexpr const char* acrobat[]{"pdf"};
    static constexpr const char* archive[]{"rar", "zip", "7z", "tar", "gz", "tgz", "bz2", "xz", "zst"};
    static constexpr const char* code[]{"c",    "cc",   "cpp",  "cxx", "h",    "hh",  "hpp",   "hxx",
                                        "cs",   "js",   "jsx",  "mjs", "ts",   "tsx", "py",    "json",
                                        "sh",   "bash", "ps1",  "bat", "cmd",  "yaml", "yml",  "lua",
                                        "rs",   "go",   "java", "kt",  "rb",   "php", "pl",    "sql",
                                        "toml", "ini",  "cfg",  "swift", "m",  "mm",  "asm",   "s",
                                        "zig",  "hs",   "dart", "vue", "glsl", "hlsl", "cmake", "diff",
                                        "patch"};
    static constexpr const char* document[]{"txt", "rtf", "doc", "docx", "md", "pages", "ppt", "pptx",
                                            "pptm", "key", "log", "odt", "odp"};
    static constexpr const char* spreadsheet[]{"xls", "xlsx", "numbers", "csv", "ods", "tsv"};
    static constexpr const char* audio[]{"mp3", "ogg", "opus", "wav", "aiff", "aif", "flac", "m4a", "aac", "wma"};
    if (extension_in(extension, webcode))
        return UiAttachmentFileKind::webcode;
    if (extension_in(extension, image))
        return UiAttachmentFileKind::image;
    if (extension_in(extension, video))
        return UiAttachmentFileKind::video;
    if (extension_in(extension, acrobat))
        return UiAttachmentFileKind::acrobat;
    if (extension_in(extension, archive))
        return UiAttachmentFileKind::archive;
    if (extension_in(extension, code))
        return UiAttachmentFileKind::code;
    if (extension_in(extension, document))
        return UiAttachmentFileKind::document;
    if (extension_in(extension, spreadsheet))
        return UiAttachmentFileKind::spreadsheet;
    if (extension_in(extension, audio))
        return UiAttachmentFileKind::audio;
    return UiAttachmentFileKind::unknown;
}

UiIcon ui_attachment_file_icon(UiAttachmentFileKind kind) noexcept {
    switch (kind) {
    case UiAttachmentFileKind::image:
        return UiIcon::image;
    case UiAttachmentFileKind::video:
        return UiIcon::file_video;
    case UiAttachmentFileKind::audio:
        return UiIcon::file_audio;
    case UiAttachmentFileKind::archive:
        return UiIcon::file_archive;
    case UiAttachmentFileKind::code:
    case UiAttachmentFileKind::webcode:
        return UiIcon::code;
    case UiAttachmentFileKind::document:
    case UiAttachmentFileKind::spreadsheet:
    case UiAttachmentFileKind::acrobat:
        return UiIcon::file_text;
    default:
        return UiIcon::file;
    }
}

u32 ui_attachment_format_size(u64 bytes, Slice<char> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    static constexpr const char* units[]{" KB", " MB", " GB", " TB"};
    if (bytes < 1024)
        return append(output, append_unsigned(output, 0, bytes), " B");
    u32 unit = 0;
    u64 scale = 1024;
    while (unit < 3 && bytes >= scale * 1024) {
        scale *= 1024;
        ++unit;
    }
    // One decimal below ten units, whole numbers above.
    const u64 tenths = (bytes * 10 + scale / 2) / scale;
    u32 used = 0;
    if (tenths < 100) {
        used = append_unsigned(output, used, tenths / 10);
        if (tenths % 10) {
            used = append(output, used, ".");
            used = append_unsigned(output, used, tenths % 10);
        }
    } else {
        used = append_unsigned(output, used, (tenths + 5) / 10);
    }
    return append(output, used, units[unit]);
}

u32 ui_attachment_format_duration(u32 milliseconds, Slice<char> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    const u32 seconds = (milliseconds + 500) / 1000;
    const u32 hours = seconds / 3600, minutes = seconds / 60 % 60, remainder = seconds % 60;
    u32 used = 0;
    if (hours) {
        used = append_unsigned(output, used, hours);
        used = append(output, used, ":");
        used = append_unsigned(output, used, minutes, 2);
    } else {
        used = append_unsigned(output, used, minutes);
    }
    used = append(output, used, ":");
    return append_unsigned(output, used, remainder, 2);
}

float ui_composer_attachment_strip_content_width(u32 count, const UiComposerAttachmentStripStyle& style) noexcept {
    if (!count)
        return 0;
    return style.leading + static_cast<float>(count) * style.tile + static_cast<float>(count - 1) * style.gap +
           style.trailing;
}

float ui_composer_attachment_strip_clamp_scroll(UiRect strip, u32 count, float scroll,
                                                const UiComposerAttachmentStripStyle& style) noexcept {
    const float overflow =
        maximum(0.f, ui_composer_attachment_strip_content_width(count, style) - ui_rect_width(strip));
    return scroll != scroll ? 0.f : minimum(overflow, maximum(0.f, scroll));
}

float ui_composer_attachment_strip_reveal(UiRect strip, u32 count, u32 index, float scroll,
                                          const UiComposerAttachmentStripStyle& style) noexcept {
    if (index >= count)
        return ui_composer_attachment_strip_clamp_scroll(strip, count, scroll, style);
    const float left = style.leading + static_cast<float>(index) * (style.tile + style.gap);
    // The action bar overhangs the tile's end edge by a quarter of its width.
    const float right = left + style.tile + style.gap;
    const float width = ui_rect_width(strip);
    if (left - style.leading < scroll)
        scroll = left - style.leading;
    else if (right > scroll + width)
        scroll = right - width;
    return ui_composer_attachment_strip_clamp_scroll(strip, count, scroll, style);
}

UiComposerAttachmentTileGeometry ui_composer_attachment_tile_geometry(
    UiRect strip, const UiComposerAttachmentTile& tile, u32 index, float scroll,
    const UiComposerAttachmentStripStyle& style) noexcept {
    UiComposerAttachmentTileGeometry geometry{};
    const float left = strip.left + style.leading - scroll + static_cast<float>(index) * (style.tile + style.gap);
    const float top = strip.top + style.top;
    geometry.card = {left, top, left + style.tile, top + style.tile};
    const UiRect inner = ui_rect_shrink(geometry.card, style.padding);
    geometry.name = {inner.left, inner.bottom - style.name_height, inner.right, inner.bottom};
    geometry.media = {inner.left, inner.top, inner.right, geometry.name.top - style.name_gap};
    char status[24];
    const float size_width = minimum(ui_rect_width(inner) * .5f, status_width(status_text(tile, {status, 24})));
    geometry.size = {geometry.name.right - size_width, geometry.name.top, geometry.name.right, geometry.name.bottom};
    if (size_width > 0)
        geometry.name.right = geometry.size.left - 4;
    if (tile.phase == UiComposerAttachmentPhase::uploading || tile.phase == UiComposerAttachmentPhase::waiting)
        geometry.progress = {geometry.media.left + 6, geometry.media.bottom - 6 - style.progress,
                             geometry.media.right - 6, geometry.media.bottom - 6};
    if (tile.phase == UiComposerAttachmentPhase::preparing)
        return geometry;
    if (tile.can_retry)
        geometry.parts[geometry.button_count++] = UiComposerAttachmentPart::retry;
    geometry.parts[geometry.button_count++] = UiComposerAttachmentPart::spoiler;
    geometry.parts[geometry.button_count++] = UiComposerAttachmentPart::edit;
    geometry.parts[geometry.button_count++] = UiComposerAttachmentPart::remove;
    const float bar_width =
        static_cast<float>(geometry.button_count) * style.action + style.action_padding * 2;
    const float bar_height = style.action + style.action_padding * 2;
    // The bar is offset by (25%, -25%) of its own size.
    const float bar_right = geometry.card.right + bar_width * .25f;
    const float bar_top = geometry.card.top - bar_height * .25f;
    geometry.actions = {bar_right - bar_width, bar_top, bar_right, bar_top + bar_height};
    for (u32 i = 0; i < geometry.button_count; ++i) {
        const float x = geometry.actions.left + style.action_padding + static_cast<float>(i) * style.action;
        const float y = geometry.actions.top + style.action_padding;
        geometry.buttons[i] = {x, y, x + style.action, y + style.action};
    }
    return geometry;
}

UiRect ui_composer_attachment_fit(UiRect media, u32 width, u32 height) noexcept {
    if (!width || !height || !ui_rect_has_area(media))
        return {};
    const float scale = minimum(2.f, minimum(ui_rect_width(media) / static_cast<float>(width),
                                             ui_rect_height(media) / static_cast<float>(height)));
    const float w = static_cast<float>(width) * scale, h = static_cast<float>(height) * scale;
    const float x = (media.left + media.right - w) * .5f, y = (media.top + media.bottom - h) * .5f;
    return {x, y, x + w, y + h};
}

float ui_composer_attachment_thumbnail_blur(const UiComposerAttachmentTile& tile, UiRect fitted) noexcept {
    if (!tile.spoiler || !ui_rect_has_area(fitted))
        return 0;
    return minimum(kUiComposerAttachmentSpoilerBlur, minimum(ui_rect_width(fitted), ui_rect_height(fitted)) * .25f);
}

u32 ui_composer_attachment_tile_tags(const UiComposerAttachmentTile& tile, UiRect media,
                                     Slice<UiComposerAttachmentTag> output, Slice<char> duration_storage,
                                     const UiComposerAttachmentStripStyle& style) noexcept {
    if (!output.data || tile.phase == UiComposerAttachmentPhase::preparing)
        return 0;
    Utf8View labels[kUiComposerAttachmentTags]{};
    u32 count = 0;
    if (tile.kind == UiAttachmentFileKind::video && tile.duration_ms && duration_storage.data) {
        const u32 bytes = ui_attachment_format_duration(tile.duration_ms, duration_storage);
        if (bytes)
            labels[count++] = {duration_storage.data, bytes};
    } else if (tile.animated) {
        labels[count++] = utf8_literal("GIF");
    }
    if (tile.described)
        labels[count++] = utf8_literal("ALT");
    if (tile.spoiler)
        labels[count++] = utf8_literal("SPOILER");
    u32 written = 0;
    float x = media.left + 6;
    const float bottom = media.bottom - 6 - (tile.phase == UiComposerAttachmentPhase::uploading ||
                                                     tile.phase == UiComposerAttachmentPhase::waiting
                                                 ? style.progress + 4
                                                 : 0);
    for (u32 i = 0; i < count && written < output.count; ++i) {
        const float width = tag_width(labels[i], style.tag_height);
        if (x + width > media.right - 6)
            break;
        output[written++] = {{x, bottom - style.tag_height, x + width, bottom}, labels[i]};
        x += width + 4;
    }
    return written;
}

UiComposerAttachmentHit ui_composer_attachment_strip_hit(UiRect strip, ConstSlice<UiComposerAttachmentTile> tiles,
                                                         float scroll, float x, float y,
                                                         const UiComposerAttachmentStripStyle& style) noexcept {
    if (!tiles.data || !ui_rect_contains(strip, x, y))
        return {kUiComposerAttachmentNoTile, UiComposerAttachmentPart::none};
    // Later action bars overlap earlier gaps; buttons take precedence over cards.
    for (u32 i = 0; i < tiles.count; ++i) {
        const auto geometry = ui_composer_attachment_tile_geometry(strip, tiles[i], i, scroll, style);
        for (u32 b = 0; b < geometry.button_count; ++b)
            if (ui_rect_contains(geometry.buttons[b], x, y))
                return {i, geometry.parts[b]};
    }
    for (u32 i = 0; i < tiles.count; ++i) {
        const auto geometry = ui_composer_attachment_tile_geometry(strip, tiles[i], i, scroll, style);
        if (ui_rect_contains(geometry.card, x, y) || ui_rect_contains(geometry.actions, x, y))
            return {i, UiComposerAttachmentPart::tile};
    }
    return {kUiComposerAttachmentNoTile, UiComposerAttachmentPart::none};
}

bool ui_composer_attachment_thumbnail_visible(const UiComposerAttachmentStripView& view, u32 index, UiRect* media,
                                              UiRect* clip, const UiComposerAttachmentStripStyle& style) noexcept {
    if (index >= view.tiles.count || !shows_thumbnail(view.tiles[index]))
        return false;
    const auto geometry =
        ui_composer_attachment_tile_geometry(view.strip, view.tiles[index], index, view.scroll, style);
    const UiRect visible = ui_rect_intersection(geometry.media, view.strip);
    if (!ui_rect_has_area(visible))
        return false;
    if (media)
        *media = geometry.media;
    if (clip)
        *clip = visible;
    return true;
}

bool ui_composer_attachment_strip_paint_base(const UiComposerAttachmentStripView& view, const Theme& theme,
                                             PaintStream* paint,
                                             const UiComposerAttachmentStripStyle& style) noexcept {
    if (!paint || (!view.tiles.data && view.tiles.count))
        return false;
    if (!view.tiles.count || !ui_rect_has_area(view.strip))
        return true;
    if (!clip_push(paint, view.strip))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < view.tiles.count; ++i) {
        const auto& tile = view.tiles[i];
        const auto geometry = ui_composer_attachment_tile_geometry(view.strip, tile, i, view.scroll, style);
        if (!ui_rect_has_area(ui_rect_intersection(geometry.card, view.strip)))
            continue;
        const bool hovered = view.hovered == i;
        const Color border = tile.phase == UiComposerAttachmentPhase::failed ? theme.tokens.accent.danger
                             : hovered                                       ? theme.tokens.border.strong
                                                                             : theme.tokens.border.subtle;
        ok = ui_background_paint_surface(paint, geometry.card, theme, theme.surface, border, theme.scale.radius_sm, 1,
                                         .9f, UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                                         UiBackgroundBorderTone::semantic) &&
             paint_stream_push_rounded(paint, geometry.media.left, geometry.media.top, geometry.media.right,
                                       geometry.media.bottom, theme.base_lowest, theme.scale.radius_sm);
        if (ok && view.owner.manager)
            ok = paint_stream_record_content(paint, view.owner, ui_rect_intersection(geometry.card, view.strip),
                                             UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap);
        if (ok && !shows_thumbnail(tile)) {
            const Color ink = tile.spoiler ? theme.text_primary : ui_background_muted_text_color(theme);
            ok = ui_icon_paint(paint, placeholder_icon(tile), centered(geometry.media, style.icon),
                               ui_foreground_color(theme, ink));
        }
    }
    return paint_stream_pop_clip(paint) && ok;
}

bool ui_composer_attachment_strip_paint_overlay(const UiComposerAttachmentStripView& view, const Theme& theme,
                                                PaintStream* paint,
                                                const UiComposerAttachmentStripStyle& style) noexcept {
    if (!paint || (!view.tiles.data && view.tiles.count))
        return false;
    if (!view.tiles.count || !ui_rect_has_area(view.strip))
        return true;
    if (!clip_push(paint, view.strip))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < view.tiles.count; ++i) {
        const auto& tile = view.tiles[i];
        const auto geometry = ui_composer_attachment_tile_geometry(view.strip, tile, i, view.scroll, style);
        if (!ui_rect_has_area(ui_rect_intersection(geometry.card, view.strip)) &&
            !ui_rect_has_area(ui_rect_intersection(geometry.actions, view.strip)))
            continue;
        UiComposerAttachmentTag tags[kUiComposerAttachmentTags];
        char duration[16];
        const u32 tag_count =
            ui_composer_attachment_tile_tags(tile, geometry.media, {tags, kUiComposerAttachmentTags}, {duration, 16}, style);
        for (u32 t = 0; ok && t < tag_count; ++t)
            ok = paint_stream_push_rounded(paint, tags[t].rectangle.left, tags[t].rectangle.top,
                                           tags[t].rectangle.right, tags[t].rectangle.bottom,
                                           theme.tokens.overlay.dense, theme.scale.radius_xs);
        if (ok && ui_rect_has_area(geometry.progress)) {
            const float fraction =
                tile.bytes ? minimum(1.f, static_cast<float>(static_cast<double>(tile.uploaded_bytes) /
                                                             static_cast<double>(tile.bytes)))
                           : 0.f;
            const auto& bar = geometry.progress;
            ok = paint_stream_push_rounded(paint, bar.left, bar.top, bar.right, bar.bottom, theme.tokens.overlay.dense,
                                           theme.scale.radius_full);
            if (ok && fraction > 0)
                ok = paint_stream_push_rounded(paint, bar.left, bar.top, bar.left + ui_rect_width(bar) * fraction,
                                               bar.bottom, theme.tokens.emphasis.fill, theme.scale.radius_full);
        }
        if (ok && view.selected == i && view.focus_visible)
            ok = paint_stream_push_rounded_border(paint, geometry.card.left - 2, geometry.card.top - 2,
                                                  geometry.card.right + 2, geometry.card.bottom + 2, Color{},
                                                  theme.tokens.border.focus, theme.scale.radius_sm + 2, 2);
        if (ok && geometry.button_count) {
            ok = ui_background_paint_surface(paint, geometry.actions, theme, theme.base_lower,
                                             theme.tokens.border.subtle, theme.scale.radius_sm, 1, .9f,
                                             UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                                             UiBackgroundBorderTone::semantic);
            if (ok && view.owner.manager)
                ok = paint_stream_record_content(paint, view.owner, ui_rect_intersection(geometry.actions, view.strip),
                                                 UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap);
            for (u32 b = 0; ok && b < geometry.button_count; ++b) {
                const auto part = geometry.parts[b];
                const bool hot = view.hovered == i && view.hovered_part == part;
                const UiRect button = geometry.buttons[b];
                if (hot)
                    ok = paint_stream_push_rounded(paint, button.left, button.top, button.right, button.bottom,
                                                   theme.tokens.state.hover, theme.scale.radius_xs);
                const Color ink = part == UiComposerAttachmentPart::remove && hot ? theme.tokens.text.danger
                                  : part == UiComposerAttachmentPart::spoiler && tile.spoiler
                                      ? theme.tokens.text.link
                                  : hot ? theme.text_primary
                                        : ui_background_muted_text_color(theme);
                if (ok)
                    ok = ui_icon_paint(paint, button_icon(tile, part), centered(button, ui_icon_button_extent(20)),
                                       ui_foreground_color(theme, ink));
            }
        }
    }
    return paint_stream_pop_clip(paint) && ok;
}

bool ui_composer_attachment_strip_text(const UiComposerAttachmentStripView& view, const Theme& theme,
                                       UiTextOutput* output, const UiComposerAttachmentStripStyle& style) noexcept {
    if (!output || (!view.tiles.data && view.tiles.count))
        return false;
    for (u32 i = 0; i < view.tiles.count; ++i) {
        const auto& tile = view.tiles[i];
        const auto geometry = ui_composer_attachment_tile_geometry(view.strip, tile, i, view.scroll, style);
        const UiRect visible = ui_rect_intersection(geometry.card, view.strip);
        if (!ui_rect_has_area(visible))
            continue;
        const Utf8View name = tile.phase == UiComposerAttachmentPhase::preparing && !tile.filename.bytes
                                  ? utf8_literal("Preparing files\xE2\x80\xA6")
                                  : tile.filename;
        const UiRect name_clip = ui_rect_intersection(geometry.name, view.strip);
        if (name.bytes && ui_rect_has_area(name_clip) &&
            !ui_text_output_push(output, {name, view.owner, geometry.name, name_clip, theme.text_primary,
                                          text_style_subtext, UiTextAlignment::start, UiTextAlignment::center,
                                          UiTextWrapping::no_wrap, UiTextOverflow::ellipsis}))
            return false;
        char status[24];
        const u32 status_bytes = status_text(tile, {status, 24});
        const UiRect size_clip = ui_rect_intersection(geometry.size, view.strip);
        if (status_bytes && ui_rect_has_area(size_clip) &&
            !ui_text_output_push_owned(
                output, {{status, status_bytes}, view.owner, geometry.size, size_clip,
                         tile.phase == UiComposerAttachmentPhase::failed ? theme.tokens.text.danger
                                                                         : ui_background_muted_text_color(theme),
                         text_style_subtext, UiTextAlignment::end, UiTextAlignment::center, UiTextWrapping::no_wrap,
                         UiTextOverflow::clip}))
            return false;
        UiComposerAttachmentTag tags[kUiComposerAttachmentTags];
        char duration[16];
        const u32 tag_count =
            ui_composer_attachment_tile_tags(tile, geometry.media, {tags, kUiComposerAttachmentTags}, {duration, 16}, style);
        for (u32 t = 0; t < tag_count; ++t) {
            const UiRect clip = ui_rect_intersection(tags[t].rectangle, view.strip);
            if (!ui_rect_has_area(clip))
                continue;
            if (!ui_text_output_push_owned(output, {tags[t].text, view.owner, tags[t].rectangle, clip,
                                                    theme.tokens.text.on_accent, text_style_subtext | text_style_bold,
                                                    UiTextAlignment::center, UiTextAlignment::center,
                                                    UiTextWrapping::no_wrap, UiTextOverflow::clip}))
                return false;
        }
    }
    return true;
}

} // namespace maicord
