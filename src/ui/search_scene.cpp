#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/menu_row.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/attachment_actions.hpp"
#include "maicord/ui/audio_controls.hpp"
#include "maicord/model/message_poll_store.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
enum class UiSearchItemKind : u8 {
    attachment,
    embed,
    content_link
};
struct UiSearchItem {
    UiRect rectangle; // body-relative, before scrolling
    u64 message_id, channel_id;
    u32 source_index, text_offset, text_bytes;
    u16 rich_index;
    UiSearchItemKind kind;
    MediaEmbedResource resource;
    bool snapshot;
};
struct UiSearchRichProjection {
    MessageScene scene;
    UiWidgetStorageOwner widgets;
    MessageTimeline* timeline;
    u32 source_revision, height_updates, layout_passes;
    UiSearchItem items[kUiSearchItemMaximum];
    const UiSearchSource* item_source;
    u32 item_count, item_revision, visible_count;
    float item_width;
    UiSearchLayout item_layout;
    u16 visible[kUiSearchVisibleItems];
    UiWidgetHandle primary[kUiSearchVisibleItems], secondary[kUiSearchVisibleItems];
    UiRect screens[kUiSearchVisibleItems];
    char details[kUiSearchVisibleItems][16];
    u8 detail_bytes[kUiSearchVisibleItems];
};
namespace {
constexpr float default_body_height = 48;
UiRect rich_item_clip(const UiSearchScene&, UiRect, UiRect) noexcept;
bool rich_enabled(const UiSearchScene& s) noexcept {
    return s.source && s.source->rich_timeline;
}
UiRect rich_viewport(const UiSearchScene& s) noexcept {
    return {s.body.left + 8, s.body.top, s.body.right - 20, s.body.bottom};
}
void configure_rich_input(MessageScene& scene, const UiSearchSource& source) noexcept {
    for (u32 i = 0; i < scene.count; ++i) {
        auto& outer = scene.rows[i];
        UiSearchRow authority{};
        const bool redacted =
            !source.resolve(source.context, outer.source_index, &authority) || authority.unavailable;
        const auto* snapshot = message_scene_snapshot_row(scene, outer);
        MessageSceneRowWidget* rows[]{&outer,
                                      snapshot ? &scene.snapshot_rows.data[outer.snapshot_slot] : nullptr};
        for (u32 child = 0; child < 2; ++child) {
            auto* row = rows[child];
            if (!row)
                continue;
            if (row->redacted != redacted) {
                row->redacted = redacted;
                scene.dirty = true;
                ++scene.data_updates;
            }
            if (auto* widget = ui_widget_manager_resolve(&scene.widgets, row->widget)) {
                if (redacted)
                    widget->flags |= ui_widget_block_input;
                else
                    widget->flags &= static_cast<u16>(~ui_widget_block_input);
            }
            if (auto* widget = ui_widget_manager_resolve(&scene.widgets, row->poll)) {
                if (redacted)
                    widget->flags |= ui_widget_block_input;
                else
                    widget->flags &= static_cast<u16>(~ui_widget_block_input);
            }
            if (redacted)
                continue;
            const auto* rich =
                child ? message_rich_store_snapshot(*scene.timeline->rich_store, row->message_id)
                      : message_rich_store_find(*scene.timeline->rich_store, row->message_id);
            if (!rich)
                continue;
            for (u32 j = 0; j < rich->embed_count; ++j) {
                MessageRichEmbedMediaSelection selected{};
                if (!message_rich_embed_media_select(*scene.timeline->rich_store, rich->embeds[j],
                                                     &selected) &&
                    !message_rich_embed_resource_select(*scene.timeline->rich_store, rich->embeds[j],
                                                        MediaEmbedResource::thumbnail, &selected))
                    continue;
                auto* widget = ui_widget_manager_resolve(
                    &scene.widgets,
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::embed, j));
                if (widget) {
                    widget->kind = UiWidgetKind::button;
                    widget->flags |= ui_widget_hit_test | ui_widget_focusable;
                }
            }
            for (u32 j = 0; j < rich->component_count; ++j) {
                if (!rich->components[j].disabled)
                    continue;
                auto* widget = ui_widget_manager_resolve(
                    &scene.widgets,
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::component, j));
                if (widget)
                    widget->flags &= static_cast<u16>(~(ui_widget_hit_test | ui_widget_focusable));
            }
        }
    }
}
bool bind_rich(UiSearchScene& s) noexcept {
    if (!rich_enabled(s))
        return true;
    auto* timeline = s.source->rich_timeline;
    if (!timeline->store || !timeline->rich_store || !timeline->list ||
        timeline->store->count > s.source->count || timeline->list->count != s.source->count ||
        (!s.source->rich_order.count && timeline->store->count != s.source->count))
        return false;
    if (!s.rich_projection) {
        if (!arena_initialize(&s.rich_projection_storage, sizeof(UiSearchRichProjection), 0, 4096))
            return false;
        s.rich_projection = arena_allocate_zero<UiSearchRichProjection>(&s.rich_projection_storage);
        if (!s.rich_projection)
            return false;
    }
    auto& p = *s.rich_projection;
    if (!p.widgets.widgets.arena.base &&
        !ui_widget_storage_initialize(&p.widgets, 1 + kUiSearchVisibleRows * kMessageSceneWidgetsPerRow,
                                      64))
        return false;
    if (p.timeline != timeline || p.source_revision != s.source->revision) {
        if ((!s.source->rich_order.data && s.source->rich_order.count) ||
            (s.source->rich_order.count && s.source->rich_order.count != s.source->count))
            return false;
        for (u32 i = 0; i < s.source->count; ++i) {
            const auto handle = s.source->rich_order.count
                                    ? s.source->rich_order[i]
                                    : message_store_ordered_handle(*timeline->store, i);
            const auto* record = message_store_resolve(*timeline->store, handle);
            UiSearchRow row{};
            if (!record || !s.source->resolve(s.source->context, i, &row) || row.message_id != record->id ||
                row.channel_id != record->channel_id)
                return false;
        }
        message_scene_release(&p.scene);
        p.timeline = nullptr;
        if (!message_scene_initialize_owned(&p.scene, timeline, kUiSearchVisibleRows, 4,
                                            ui_widget_storage_widgets(p.widgets),
                                            ui_widget_storage_layout_nodes(p.widgets)) ||
            !ui_widget_manager_bind_growable_storage(&p.scene.widgets, ui_widget_storage_grow,
                                                     &p.widgets) ||
            !message_scene_set_result_card_style(&p.scene, &kDefaultMessageResultCardStyle) ||
            !message_scene_set_source_order(&p.scene, s.source->rich_order, s.source->revision))
            return false;
        p.timeline = timeline;
        p.source_revision = s.source->revision;
    }
    message_scene_bind_theme_styles(&p.scene, s.theme_styles);
    message_scene_set_media_policy(&p.scene, s.media_policy);
    // Item layouts own their scroll; the hidden message projection stays at the top.
    const bool items = s.layout != UiSearchLayout::list;
    if (!message_scene_bind_poll_source(&p.scene, s.source->polls) ||
        !message_timeline_set_scroll(timeline, items ? 0 : ui_extent_units(s.scroll_dips), false) ||
        !message_scene_update(&p.scene, rich_viewport(s)).ok())
        return false;
    configure_rich_input(p.scene, *s.source);
    if (!items)
        s.scroll_dips = ui_extent_dips(timeline->scroll_units);
    return true;
}
bool chooser_open(const UiSearchScene& s) noexcept {
    return s.content->filter_choices || s.content->suggestions || s.content->filters;
}
bool recent_shown(const UiSearchScene& s) noexcept {
    return !s.choice_source && !s.content->filters && s.content->recent_count && s.content->recent &&
           !s.content->edit.text.bytes;
}
float recent_top(const UiSearchScene& s) noexcept {
    return s.chooser.top + 28 + 5 * 48 + 4;
}
bool choices_authorized(const UiSearchFilterChoiceSource* source) noexcept {
    return !source ||
           (source->revision && source->rows.count <= kUiSearchFilterChoiceMaximum &&
            static_cast<u8>(source->kind) <= static_cast<u8>(UiSearchFilterKind::file_name) &&
            (!source->rows.count || source->rows.data) &&
            (source->selected == kUiSearchFilterNoSelection || source->selected < source->rows.count) &&
            (!source->current || source->current(source->context)));
}
bool choices_valid(const UiSearchFilterChoiceSource* source) noexcept {
    if (!source)
        return true;
    if (!choices_authorized(source) || source->heading.bytes > 256 || source->hint.bytes > 512 ||
        !utf8_validate(source->heading).valid || !utf8_validate(source->hint).valid)
        return false;
    for (u32 index = 0; index < source->rows.count; ++index) {
        const auto& choice = source->rows[index];
        if (!choice.label.bytes || choice.label.bytes > 512 || choice.detail.bytes > 512 ||
            !choice.value.bytes || choice.value.bytes > kUiSearchFilterQueryBytes ||
            !utf8_validate(choice.label).valid || !utf8_validate(choice.detail).valid ||
            !utf8_validate(choice.value).valid)
            return false;
    }
    return true;
}
UiIcon choice_icon(const UiSearchFilterChoiceSource& source, const UiSearchFilterChoiceView& row) noexcept {
    switch (source.kind) {
    case UiSearchFilterKind::from:
        return UiIcon::user;
    case UiSearchFilterKind::mentions:
        return UiIcon::mention;
    case UiSearchFilterKind::in:
        return UiIcon::channel;
    case UiSearchFilterKind::pinned:
        return UiIcon::pin;
    case UiSearchFilterKind::link_from:
        return UiIcon::link;
    case UiSearchFilterKind::file_name:
        return UiIcon::file_text;
    case UiSearchFilterKind::author_type:
        return row.semantic == 1 ? UiIcon::bot : row.semantic == 2 ? UiIcon::webhook : UiIcon::user;
    case UiSearchFilterKind::file_type: {
        constexpr UiIcon icons[] = {UiIcon::image, UiIcon::file_video, UiIcon::file_audio,
                                    UiIcon::file_text, UiIcon::file_archive};
        return row.semantic < 5 ? icons[row.semantic] : UiIcon::file;
    }
    case UiSearchFilterKind::has:
        switch (row.semantic) {
        case 0:
            return UiIcon::link;
        case 3:
            return UiIcon::play;
        case 5:
            return UiIcon::speaker;
        case 6:
            return UiIcon::sticker;
        case 8:
            return UiIcon::forwarded;
        default:
            return UiIcon::attachment;
        }
    default:
        return UiIcon::search;
    }
}
bool choices_current(const UiSearchScene& scene) noexcept {
    const auto* source = scene.content ? scene.content->filter_choices : nullptr;
    return source == scene.choice_source &&
           (!source ||
            (source->revision == scene.choice_source_revision && source->rows.data == scene.choice_rows &&
             source->rows.count == scene.choice_total && source->selected == scene.choice_selected &&
             (!source->current || source->current(source->context))));
}
float choice_em(const UiSearchScene& scene) noexcept {
    return scene.theme_styles && scene.theme_styles->ready ? scene.theme_styles->typography.body_size
                                                           : 16.f;
}
float choice_heading_height(const UiSearchScene& scene) noexcept {
    return maximum(ui_theme_metrics(scene.theme_styles).control_height, choice_em(scene) * 1.3125f);
}
float choice_hint_height(const UiSearchScene& scene) noexcept {
    if (scene.choice_total && !scene.choice_source->hint.bytes)
        return 0;
    const float ratio = scene.theme_styles && scene.theme_styles->ready
                            ? scene.theme_styles->typography.subtext_ratio
                            : .8125f;
    return maximum(ui_theme_metrics(scene.theme_styles).menu_compact_height,
                   choice_em(scene) * ratio * 2 * 1.3125f);
}
float choice_row_height(const UiSearchScene& scene) noexcept {
    const auto& metrics = ui_theme_metrics(scene.theme_styles);
    const float line = choice_em(scene) * 1.3125f;
    return maximum(scene.choice_details ? metrics.menu_detail_height : metrics.menu_row_height,
                   line * (scene.choice_details ? 2.f : 1.f) + 2 * metrics.menu_padding);
}
UiRect choice_rows_rectangle(const UiSearchScene& scene) noexcept {
    const auto& metrics = ui_theme_metrics(scene.theme_styles);
    return {scene.chooser.left + metrics.menu_padding,
            scene.chooser.top + metrics.menu_padding + choice_heading_height(scene),
            scene.chooser.right - metrics.menu_padding,
            scene.chooser.bottom - metrics.menu_padding - choice_hint_height(scene)};
}
bool choice_menu_geometry(const UiSearchScene& scene, UiRect rectangle,
                          UiMenuRowGeometry* geometry) noexcept {
    const auto& metrics = ui_theme_metrics(scene.theme_styles);
    const UiRect content{rectangle.left + metrics.menu_padding, rectangle.top + metrics.menu_padding,
                         rectangle.right - metrics.menu_padding, rectangle.bottom - metrics.menu_padding};
    return ui_menu_row_project(content, ui_rect_intersection(rectangle, scene.chooser),
                               ui_menu_row_presentation(UiIcon::search, scene.choice_details),
                               scene.choice_details, false, geometry, scene.theme_styles);
}
// People suggestions (from:, mentions:) show the user's avatar where other
// filters show an icon: a circle centred on the icon slot, as tall as the row.
bool choice_person(const UiSearchFilterChoiceSource& source, const UiSearchFilterChoiceView& row) noexcept {
    return row.id && (source.kind == UiSearchFilterKind::from || source.kind == UiSearchFilterKind::mentions);
}
UiRect choice_avatar_rectangle(const UiMenuRowGeometry& geometry, UiRect row) noexcept {
    const float cx = (geometry.icon.left + geometry.icon.right) * .5f;
    const float cy = (row.top + row.bottom) * .5f;
    const float half = minimum(12.f, maximum(ui_rect_width(geometry.icon), ui_rect_height(geometry.icon)) * .5f + 2.f);
    return {cx - half, cy - half, cx + half, cy + half};
}
// Tab presentation and the per-item layouts of the Media, Files and Links tabs.
Utf8View tab_label(u32 tab) noexcept {
    constexpr Utf8View labels[] = {utf8_literal("Messages"), utf8_literal("Media"), utf8_literal("Files"),
                                   utf8_literal("Links"), utf8_literal("Pins")};
    return tab < 5 ? labels[tab] : Utf8View{};
}
u8 compact_count(char* out, u64 value) noexcept {
    u8 used = static_cast<u8>(decimal_write(out, value < 1000 ? value : value / 1000));
    if (value >= 1000 && value < 10000) {
        out[used++] = '.';
        out[used++] = static_cast<char>('0' + value / 100 % 10);
    }
    if (value >= 1000)
        out[used++] = 'k';
    return used;
}
u8 size_text(char* out, u64 bytes) noexcept {
    constexpr const char* units[] = {"B", "KB", "MB", "GB"};
    u32 unit = 0;
    u64 scaled = bytes * 10;
    while (scaled >= 10240 && unit < 3) {
        scaled /= 1024;
        ++unit;
    }
    char digits[24];
    u8 used = 0, n = 0;
    u64 whole = unit ? scaled / 10 : bytes;
    do {
        digits[n++] = static_cast<char>('0' + whole % 10);
        whole /= 10;
    } while (whole && n < 20);
    while (n)
        out[used++] = digits[--n];
    if (unit && scaled < 100 && scaled % 10) {
        out[used++] = '.';
        out[used++] = static_cast<char>('0' + scaled % 10);
    }
    out[used++] = ' ';
    for (const char* u = units[unit]; *u; ++u)
        out[used++] = *u;
    return used;
}
Utf8View url_hostname(Utf8View url) noexcept {
    u32 at = 0;
    while (at + 2 < url.bytes &&
           !(url.data[at] == ':' && url.data[at + 1] == '/' && url.data[at + 2] == '/'))
        ++at;
    if (at + 2 >= url.bytes)
        return {};
    at += 3;
    u32 end = at;
    while (end < url.bytes && url.data[end] != '/' && url.data[end] != '?' && url.data[end] != '#' &&
           url.data[end] != ':')
        ++end;
    return {url.data + at, end - at};
}
// First http(s) link in plain message text; the text is borrowed from the result bank.
bool content_link(Utf8View text, u32* offset, u32* bytes) noexcept {
    for (u32 at = 0; at + 8 <= text.bytes; ++at) {
        const bool secure = bytes_equal(text.data + at, "https://", 8);
        if (!secure && !bytes_equal(text.data + at, "http://", 7))
            continue;
        u32 end = at + (secure ? 8 : 7);
        const u32 host = end;
        while (end < text.bytes && static_cast<u8>(text.data[end]) > ' ' && text.data[end] != '>' &&
               text.data[end] != '|' && text.data[end] != ')')
            ++end;
        if (end == host)
            continue;
        *offset = at;
        *bytes = end - at;
        return true;
    }
    return false;
}
MessageHandle source_handle(const UiSearchScene& s, u32 index) noexcept {
    return s.source->rich_order.count
               ? s.source->rich_order[index]
               : message_store_ordered_handle(*s.source->rich_timeline->store, index);
}
const MessageRichRecord* item_rich(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const auto& store = *s.source->rich_timeline->rich_store;
    return item.snapshot ? message_rich_store_snapshot(store, item.message_id)
                         : message_rich_store_find(store, item.message_id);
}
const MessageRichAttachment* item_attachment(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const auto* rich = item.kind == UiSearchItemKind::attachment ? item_rich(s, item) : nullptr;
    return rich && item.rich_index < rich->attachment_count ? &rich->attachments[item.rich_index] : nullptr;
}
const MessageRichEmbed* item_embed(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const auto* rich = item.kind == UiSearchItemKind::embed ? item_rich(s, item) : nullptr;
    return rich && item.rich_index < rich->embed_count ? &rich->embeds[item.rich_index] : nullptr;
}
MessageMediaObscure item_obscure(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const auto& timeline = *s.source->rich_timeline;
    const auto* record = message_store_resolve(*timeline.store, source_handle(s, item.source_index));
    if (!record || !s.rich_projection)
        return MessageMediaObscure::none;
    bool spoiler = false, explicit_media = false;
    MessageMediaSource source{};
    if (const auto* attachment = item_attachment(s, item)) {
        source = MessageMediaSource::attachment;
        spoiler = message_rich_attachment_spoiler(*timeline.rich_store, *attachment);
        explicit_media = message_rich_attachment_explicit(*attachment);
    } else if (const auto* embed = item_embed(s, item)) {
        source = MessageMediaSource::embed;
        explicit_media = message_rich_embed_explicit(*embed);
    } else {
        return MessageMediaObscure::none;
    }
    return message_scene_media_obscure(
        s.rich_projection->scene,
        message_media_owner(*record, message_rich_store_find(*timeline.rich_store, record->id)), record->id,
        message_media_reveal_key(record->id, source, item.rich_index, item.snapshot), spoiler,
        explicit_media);
}
UiSearchLayout layout_for(const UiSearchSource* source, const UiSearchContent& content) noexcept {
    if (!source || !source->rich_timeline || !content.tabs || !content.panel)
        return UiSearchLayout::list;
    switch (content.result_tab) {
    case 1:
        return UiSearchLayout::grid;
    case 2:
        return UiSearchLayout::files;
    case 3:
        return UiSearchLayout::links;
    default:
        return UiSearchLayout::list;
    }
}
// Plain (unmeasured) result header: the timestamp gets a slot sized to its
// own text (estimated at subtext width), the author the rest, ellipsizing first.
MessageResultCardHeaderLayout plain_header(UiRect card, const UiSearchRow& row) noexcept {
    auto layout = message_result_card_header_layout(card.left + 52, card.right - 12,
                                                    static_cast<float>(row.author.bytes) * 9.f,
                                                    static_cast<float>(row.detail.bytes) * 7.f);
    // Unmeasured: the author takes all the room left of a right-aligned stamp.
    if (layout.timestamp_width > 0) {
        layout.timestamp.left = card.right - 12 - layout.timestamp_width;
        layout.name.right = layout.timestamp.left - kMessageResultCardHeaderGap;
    } else
        layout.name.right = card.right - 12;
    layout.name.top = layout.timestamp.top = card.top + 8;
    layout.name.bottom = layout.timestamp.bottom = card.top + 32;
    return layout;
}
bool tiled(const UiSearchScene& s) noexcept {
    return s.layout != UiSearchLayout::list && s.rich_projection;
}
float items_left(const UiSearchScene& s) noexcept {
    return s.body.left + 12;
}
float items_right(const UiSearchScene& s) noexcept {
    return s.body.right - 20;
}
bool build_items(UiSearchScene& s) noexcept {
    auto& p = *s.rich_projection;
    const float width = maximum(1.f, items_right(s) - items_left(s));
    if (p.item_revision == s.source->revision && p.item_layout == s.layout && p.item_width == width &&
        p.item_source == s.source)
        return true;
    p.item_count = 0;
    const auto& timeline = *s.source->rich_timeline;
    const auto& store = *timeline.rich_store;
    for (u32 i = 0; i < s.source->count && p.item_count < kUiSearchItemMaximum; ++i) {
        UiSearchRow row{};
        if (!s.source->resolve(s.source->context, i, &row))
            return false;
        if (row.unavailable || !row.hit)
            continue;
        // A message can repeat as context in another result group.
        bool seen = false;
        for (u32 j = 0; j < p.item_count && !seen; ++j)
            seen = p.items[j].message_id == row.message_id;
        if (seen)
            continue;
        const auto handle = source_handle(s, i);
        const auto* record = message_store_resolve(*timeline.store, handle);
        if (!record || record->id != row.message_id)
            return false;
        const auto push = [&](UiSearchItemKind kind, u32 index, bool snapshot) noexcept -> UiSearchItem* {
            if (p.item_count == kUiSearchItemMaximum)
                return nullptr;
            auto& item = p.items[p.item_count++];
            item = {};
            item.source_index = i;
            item.message_id = record->id;
            item.channel_id = record->channel_id;
            item.kind = kind;
            item.rich_index = static_cast<u16>(index);
            item.snapshot = snapshot;
            return &item;
        };
        u32 links = 0;
        for (u32 scope = 0; scope < 2; ++scope) {
            const auto* rich = scope ? message_rich_store_snapshot(store, record->id)
                                     : message_rich_store_find(store, record->id);
            if (!rich)
                continue;
            if (s.layout != UiSearchLayout::links)
                for (u32 a = 0; a < rich->attachment_count; ++a) {
                    const auto& attachment = rich->attachments[a];
                    float w = 0, h = 0;
                    const bool media =
                        message_rich_attachment_preview_extent(store, attachment, 400, &w, &h) &&
                        !message_rich_attachment_audio_supported(store, attachment);
                    if (!attachment.url.bytes || (s.layout == UiSearchLayout::grid && !media))
                        continue;
                    push(UiSearchItemKind::attachment, a, scope != 0);
                }
            for (u32 e = 0; e < rich->embed_count; ++e) {
                MessageRichEmbedMediaSelection selected{};
                if (s.layout == UiSearchLayout::grid &&
                    message_rich_embed_media_select(store, rich->embeds[e], &selected) &&
                    selected.format != MediaAttachmentFormat::unknown) {
                    if (auto* item = push(UiSearchItemKind::embed, e, scope != 0))
                        item->resource = selected.resource;
                } else if (s.layout == UiSearchLayout::links && rich->embeds[e].url.bytes) {
                    push(UiSearchItemKind::embed, e, scope != 0);
                    ++links;
                }
            }
        }
        u32 offset = 0, bytes = 0;
        if (s.layout == UiSearchLayout::links && !links &&
            content_link(message_store_content(*timeline.store, handle), &offset, &bytes))
            if (auto* item = push(UiSearchItemKind::content_link, 0, false)) {
                item->text_offset = offset;
                item->text_bytes = bytes;
            }
    }
    const float left = items_left(s);
    if (s.layout == UiSearchLayout::grid) {
        const u32 columns = width >= 440 ? 4u : 3u;
        constexpr float gap = 4;
        const float tile = maximum(1.f, (width - gap * (columns - 1)) / columns);
        for (u32 k = 0; k < p.item_count; ++k) {
            const float x = left + (k % columns) * (tile + gap), y = 8 + (k / columns) * (tile + gap);
            p.items[k].rectangle = {x, y, x + tile, y + tile};
        }
        const u32 rows = (p.item_count + columns - 1) / columns;
        s.content_height = 8 + rows * (tile + gap) + 4;
    } else {
        for (u32 k = 0; k < p.item_count; ++k) {
            const float y = 8 + k * 60.f;
            p.items[k].rectangle = {left, y, left + width, y + 56};
        }
        s.content_height = 8 + p.item_count * 60.f + 4;
    }
    p.item_revision = s.source->revision;
    p.item_layout = s.layout;
    p.item_width = width;
    p.item_source = s.source;
    return true;
}
UiRect item_screen(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const float dy = s.body.top - s.scroll_dips;
    return {item.rectangle.left, item.rectangle.top + dy, item.rectangle.right, item.rectangle.bottom + dy};
}
// Images fill their tile: the quad covers it and the tile clips.
UiRect tile_cover(const UiSearchScene& s, const UiSearchItem& item, UiRect tile) noexcept {
    float w = 0, h = 0;
    if (const auto* attachment = item_attachment(s, item)) {
        w = static_cast<float>(attachment->width);
        h = static_cast<float>(attachment->height);
    } else if (const auto* embed = item_embed(s, item)) {
        MessageRichEmbedMediaSelection selected{};
        if (message_rich_embed_resource_select(*s.source->rich_timeline->rich_store, *embed, item.resource,
                                               &selected) &&
            selected.media) {
            w = static_cast<float>(selected.media->width);
            h = static_cast<float>(selected.media->height);
        }
    }
    const float tw = ui_rect_width(tile), th = ui_rect_height(tile);
    if (w <= 0 || h <= 0 || tw <= 0 || th <= 0)
        return tile;
    const float scale = maximum(tw / w, th / h);
    const float cw = w * scale, ch = h * scale;
    const float cx = (tile.left + tile.right) * .5f, cy = (tile.top + tile.bottom) * .5f;
    return {cx - cw * .5f, cy - ch * .5f, cx + cw * .5f, cy + ch * .5f};
}
UiSearchTarget item_target(const UiSearchScene& s, const UiSearchItem& item, bool primary) noexcept {
    UiSearchTarget target{UiSearchAction::result, item.channel_id, item.message_id};
    target.source_revision = s.source_revision;
    target.source_index = item.source_index;
    // Files open by jumping; their secondary action saves. Links open their embed,
    // or jump when the link only lives in message text. Media activation first
    // passes through the host's reveal/block decision.
    const bool rich = s.layout == UiSearchLayout::grid    ? primary
                      : s.layout == UiSearchLayout::files ? !primary
                                                          : primary && item.kind == UiSearchItemKind::embed;
    if (rich) {
        target.action = UiSearchAction::rich_item;
        target.rich_kind = item.kind == UiSearchItemKind::embed ? MessageSceneRichItemKind::embed
                                                                : MessageSceneRichItemKind::attachment;
        target.rich_index = item.rich_index;
        target.snapshot = item.snapshot;
        target.resource = s.layout == UiSearchLayout::grid ? item.resource : MediaEmbedResource::none;
        target.save = s.layout == UiSearchLayout::files;
    }
    return target;
}
const UiSearchItem* target_item(const UiSearchScene& s, UiSearchTarget target,
                                u32* slot = nullptr) noexcept {
    if (!tiled(s))
        return nullptr;
    const auto& p = *s.rich_projection;
    for (u32 v = 0; v < p.visible_count; ++v) {
        const auto& item = p.items[p.visible[v]];
        const auto expected = item_target(s, item, true);
        if (expected.action == UiSearchAction::rich_item && item.source_index == target.source_index &&
            expected.rich_kind == target.rich_kind && item.rich_index == target.rich_index &&
            item.snapshot == target.snapshot && !target.save) {
            if (slot)
                *slot = v;
            return &item;
        }
    }
    return nullptr;
}
bool measure_rows(UiSearchScene& s) noexcept {
    if (rich_enabled(s)) {
        const auto& list = *s.source->rich_timeline->list;
        for (u32 i = 0; i <= list.count; ++i)
            s.row_offsets[i] = ui_extent_dips(ui_variable_list_prefix_units(list, i));
        s.measured_source = s.source;
        s.measured_source_revision = s.source->revision;
        s.measured_count = list.count;
        return s.layout == UiSearchLayout::list || build_items(s);
    }
    if (s.measured_source == s.source &&
        s.measured_source_revision == (s.source ? s.source->revision : 0) &&
        s.measured_count == (s.source ? s.source->count : 0))
        return true;
    s.row_offsets[0] = 0;
    const u32 count = s.source ? s.source->count : 0;
    for (u32 i = 0; i < count; ++i) {
        UiSearchRow row{};
        if (!s.source->resolve(s.source->context, i, &row))
            return false;
        // /fp:fast permits compilers to remove floating NaN comparisons. Validate
        // IEEE-754 exponent bits before any arithmetic or ordered comparisons.
        u32 height_bits = 0;
        copy_bytes(&height_bits, &row.body_height, sizeof(height_bits));
        if ((height_bits & 0x7f800000u) == 0x7f800000u || !row.channel_id || !row.message_id ||
            !utf8_validate(row.body).valid || !utf8_validate(row.author).valid ||
            !utf8_validate(row.channel).valid || !utf8_validate(row.detail).valid ||
            !ui_coordinate_valid(row.body_height) || row.body_height < 0 ||
            (row.body_height != 0 && (row.body_height < 24 || row.body_height > 464)))
            return false;
        s.row_offsets[i + 1] = s.row_offsets[i] + 48 +
                               (row.body_height ? row.body_height : default_body_height) + 8 +
                               ((row.group_first || !i) ? 28 : 0);
    }
    s.measured_source = s.source;
    s.measured_source_revision = s.source ? s.source->revision : 0;
    s.measured_count = count;
    return true;
}
UiRect widget_viewport(const UiSearchScene& s) noexcept {
    return {minimum(s.viewport.left, s.field.left), minimum(s.viewport.top, s.field.top),
            maximum(s.viewport.right, s.field.right), maximum(s.viewport.bottom, s.field.bottom)};
}
bool current(const UiSearchScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision && choices_current(s) &&
           (!s.source || (s.source->revision == s.source_revision &&
                          (!s.source->current || s.source->current(s.source->context))));
}
UiRect bounds(const UiSearchScene& s, UiWidgetHandle h) noexcept {
    const auto* n = ui_widget_manager_layout_node(s.widgets, h);
    return n ? n->rectangle : UiRect{};
}
Utf8View label(UiSearchAction a) noexcept {
    switch (a) {
    case UiSearchAction::submit:
        return utf8_literal("Search");
    case UiSearchAction::close:
        return utf8_literal("Close");
    case UiSearchAction::filters:
        return utf8_literal("Filters");
    case UiSearchAction::sort:
        return utf8_literal("Sort");
    case UiSearchAction::more_filters:
        return utf8_literal("More filters");
    case UiSearchAction::previous:
        return utf8_literal("Previous");
    case UiSearchAction::next:
        return utf8_literal("Next");
    case UiSearchAction::newest:
        return utf8_literal("Newest");
    case UiSearchAction::oldest:
        return utf8_literal("Oldest");
    case UiSearchAction::relevance:
        return utf8_literal("Most relevant");
    case UiSearchAction::filter_from:
        return utf8_literal("From a specific user");
    case UiSearchAction::filter_in:
        return utf8_literal("Sent in a channel");
    case UiSearchAction::filter_has:
        return utf8_literal("Includes a link, file or embed");
    case UiSearchAction::filter_mentions:
        return utf8_literal("Mentions a user");
    case UiSearchAction::filter_on:
        return utf8_literal("Sent on a date");
    case UiSearchAction::filter_before:
        return utf8_literal("Sent before a date");
    case UiSearchAction::filter_after:
        return utf8_literal("Sent after a date");
    case UiSearchAction::filter_pinned:
        return utf8_literal("Pinned messages");
    case UiSearchAction::filter_link_from:
        return utf8_literal("Links from a website");
    case UiSearchAction::filter_file_type:
        return utf8_literal("File type");
    case UiSearchAction::filter_file_name:
        return utf8_literal("File name");
    case UiSearchAction::filter_author_type:
        return utf8_literal("Sent by a user, bot or webhook");
    case UiSearchAction::clear_recent:
        return utf8_literal("Clear");
    default:
        return {};
    }
}
UiWidgetHandle add(UiSearchScene& s, UiRect rectangle, UiSearchTarget target, Utf8View name,
                   bool enabled = true, UiWidgetHandle parent = ui_widget_invalid_handle()) noexcept {
    auto style = ui_layout_style();
    style.width = ui_layout_fixed(ui_rect_width(rectangle));
    style.height = ui_layout_fixed(ui_rect_height(rectangle));
    const auto origin = parent.generation ? bounds(s, parent) : widget_viewport(s);
    style.margin.left = rectangle.left - origin.left;
    style.margin.top = rectangle.top - origin.top;
    style.flags |= ui_layout_node_clip_children | ui_layout_node_allow_overlap;
    const u32 id = s.widgets.count + 1;
    const auto result = ui_widget_manager_add_prevalidated(
        &s.widgets, parent.generation ? parent : s.root,
        {id,
         target.action == UiSearchAction::edit ? UiWidgetKind::text_field : UiWidgetKind::button,
         static_cast<u16>(enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                  : ui_widget_none),
         {name, style, {}, 0, id}});
    if (!result.ok())
        return ui_widget_invalid_handle();
    s.targets[result.handle.slot] = target;
    return result.handle;
}
void count_label(UiSearchScene& s) noexcept {
    auto append = [&](u64 value) noexcept {
        char digits[20];
        const u32 n = decimal_write(digits, value);
        for (u32 i = 0; i < n; ++i)
            s.count_text[s.count_bytes++] = digits[i];
    };
    s.count_bytes = 0;
    for (u32 tab = 0; tab < 5; ++tab)
        s.tab_bytes[tab] = (s.content->tab_known >> tab) & 1u
                               ? compact_count(s.tab_text[tab], s.content->tab_totals[tab])
                               : 0;
    if (s.content->phase != UiSearchPhase::ready && !s.content->stale) {
        const auto heading = s.content->phase == UiSearchPhase::loading ? utf8_literal("Searching…")
                             : s.content->phase == UiSearchPhase::indexing
                                 ? utf8_literal("Indexing messages…")
                             : s.content->phase == UiSearchPhase::failed ? utf8_literal("Search failed")
                                                                         : utf8_literal("Enter a search");
        CopyMemory(s.count_text, heading.data, heading.bytes);
        s.count_bytes = static_cast<u8>(heading.bytes);
        return;
    }
    if (s.content->total_estimated) {
        constexpr auto prefix = utf8_literal("About ");
        CopyMemory(s.count_text, prefix.data, prefix.bytes);
        s.count_bytes = static_cast<u8>(prefix.bytes);
    }
    append(s.content->total);
    const auto suffix = utf8_literal(" results");
    CopyMemory(s.count_text + s.count_bytes, suffix.data, suffix.bytes);
    s.count_bytes += static_cast<u8>(suffix.bytes);
}
UiRect overlay_clip(const UiSearchScene& s, UiRect rectangle) noexcept {
    auto region = s.body;
    if (chooser_open(s) && ui_rect_has_area(ui_rect_intersection(rectangle, s.chooser)))
        region.top = maximum(region.top, s.chooser.bottom);
    if (s.content->sort_menu && ui_rect_has_area(ui_rect_intersection(rectangle, s.sort_chooser))) {
        if (rectangle.left < s.sort_chooser.left)
            region.right = minimum(region.right, s.sort_chooser.left);
        else
            region.top = maximum(region.top, s.sort_chooser.bottom);
    }
    return ui_rect_intersection(rectangle, region);
}
UiRect item_save_rectangle(UiRect row) noexcept {
    return {row.right - 44, row.top + 12, row.right - 12, row.top + 44};
}
UiRect item_jump_rectangle(UiRect row) noexcept {
    return {row.right - 64, row.top + 16, row.right - 12, row.top + 40};
}
Utf8View item_title(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const auto& store = *s.source->rich_timeline->rich_store;
    if (const auto* attachment = item_attachment(s, item))
        return message_rich_store_string(store, attachment->filename);
    if (const auto* embed = item_embed(s, item)) {
        const auto title = message_rich_store_string(store, embed->title);
        return title.bytes ? title : message_rich_store_string(store, embed->url);
    }
    if (item.kind == UiSearchItemKind::content_link) {
        const auto content =
            message_store_content(*s.source->rich_timeline->store, source_handle(s, item.source_index));
        if (item.text_offset + item.text_bytes <= content.bytes)
            return {content.data + item.text_offset, item.text_bytes};
    }
    return {};
}
Utf8View item_link(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    if (const auto* embed = item_embed(s, item))
        return message_rich_store_string(*s.source->rich_timeline->rich_store, embed->url);
    return item.kind == UiSearchItemKind::content_link ? item_title(s, item) : Utf8View{};
}
UiIcon item_icon(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    if (s.layout == UiSearchLayout::links)
        return UiIcon::link;
    const auto* attachment = item_attachment(s, item);
    if (!attachment)
        return UiIcon::file;
    const auto& store = *s.source->rich_timeline->rich_store;
    if (message_rich_attachment_audio_supported(store, *attachment))
        return UiIcon::file_audio;
    switch (message_rich_attachment_format(store, *attachment)) {
    case MediaAttachmentFormat::mp4:
        return UiIcon::file_video;
    case MediaAttachmentFormat::unknown:
        break;
    default:
        return UiIcon::image;
    }
    const auto name = message_rich_store_string(store, attachment->filename);
    const auto ends = [&](const char* suffix, u32 bytes) noexcept {
        if (name.bytes < bytes)
            return false;
        for (u32 i = 0; i < bytes; ++i) {
            char c = name.data[name.bytes - bytes + i];
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
            if (c != suffix[i])
                return false;
        }
        return true;
    };
    if (ends(".zip", 4) || ends(".rar", 4) || ends(".7z", 3) || ends(".tar", 4) || ends(".gz", 3))
        return UiIcon::file_archive;
    if (ends(".txt", 4) || ends(".md", 3) || ends(".pdf", 4) || ends(".log", 4) || ends(".json", 5))
        return UiIcon::file_text;
    return UiIcon::file;
}
bool add_items(UiSearchScene& s) noexcept {
    auto& p = *s.rich_projection;
    p.visible_count = 0;
    const bool enabled = !chooser_open(s) && !s.content->sort_menu;
    for (u32 k = 0; k < p.item_count && p.visible_count < kUiSearchVisibleItems; ++k) {
        const auto& item = p.items[k];
        const auto screen = item_screen(s, item);
        if (screen.bottom <= s.body.top)
            continue;
        if (screen.top >= s.body.bottom)
            break;
        const u32 v = p.visible_count++;
        p.visible[v] = static_cast<u16>(k);
        p.screens[v] = screen;
        p.secondary[v] = {};
        p.detail_bytes[v] = 0;
        const auto title = item_title(s, item);
        const Utf8View name = s.layout == UiSearchLayout::grid ? utf8_literal("Open media")
                              : title.bytes                    ? title
                                                               : utf8_literal("Jump");
        p.primary[v] =
            add(s, ui_rect_intersection(screen, s.body), item_target(s, item, true), name, enabled);
        if (!p.primary[v].generation)
            return false;
        UiRect secondary{};
        Utf8View secondary_name{};
        if (s.layout == UiSearchLayout::files) {
            if (const auto* attachment = item_attachment(s, item))
                p.detail_bytes[v] = size_text(p.details[v], attachment->size);
            secondary = item_save_rectangle(screen);
            secondary_name = utf8_literal("Download");
        } else if (s.layout == UiSearchLayout::links && item.kind == UiSearchItemKind::embed) {
            secondary = item_jump_rectangle(screen);
            secondary_name = utf8_literal("Jump");
        }
        secondary = ui_rect_intersection(secondary, s.body);
        if (ui_rect_has_area(secondary)) {
            p.secondary[v] = add(s, secondary, item_target(s, item, false), secondary_name, enabled);
            if (!p.secondary[v].generation)
                return false;
        }
    }
    return true;
}
bool hovered_widget(const UiSearchScene& s, UiWidgetHandle handle) noexcept {
    return handle.generation && s.widgets.hovered.generation == handle.generation &&
           s.widgets.hovered.slot == handle.slot;
}
bool focused_widget(const UiSearchScene& s, UiWidgetHandle handle) noexcept {
    return handle.generation && s.widgets.focused.generation == handle.generation &&
           s.widgets.focused.slot == handle.slot;
}
bool paint_items(const UiSearchScene& s, const Theme& theme, PaintStream* out,
                 MessageMediaCommandStream* media) noexcept {
    const auto& p = *s.rich_projection;
    const auto& timeline = *s.source->rich_timeline;
    const auto& store = *timeline.rich_store;
    for (u32 v = 0; v < p.visible_count; ++v) {
        const auto& item = p.items[p.visible[v]];
        const auto screen = p.screens[v];
        const auto clip = overlay_clip(s, screen);
        const bool hover = hovered_widget(s, p.primary[v]), focus = focused_widget(s, p.primary[v]);
        if (s.layout == UiSearchLayout::grid) {
            const MessageMediaObscure obscure = item_obscure(s, item);
            // Tiles round with the card radius token; media quads are masked to the same corners.
            const float tile_radius = theme.scale.radius_md;
            if (!paint_stream_push_rounded(out, screen.left, screen.top, screen.right, screen.bottom,
                                           theme.surface_high, tile_radius))
                return false;
            if (message_media_obscure_notice(obscure)) {
                const float cx = (screen.left + screen.right) * .5f,
                            cy = (screen.top + screen.bottom) * .5f;
                if (!ui_icon_paint(out, UiIcon::eye_off, {cx - 12, cy - 12, cx + 12, cy + 12},
                                   theme.text_secondary))
                    return false;
            } else if (media && media->storage.data && media->count < media->storage.count &&
                       ui_rect_has_area(clip)) {
                const auto* record =
                    message_store_resolve(*timeline.store, source_handle(s, item.source_index));
                MessageMediaCommand command{};
                command.rectangle = tile_cover(s, item, screen);
                command.clip = clip;
                command.owner = {&s.widgets, p.primary[v]};
                command.message_revision = record ? record->revision : 0;
                command.snapshot = item.snapshot;
                command.search_result = true;
                command.tile_radius = static_cast<u8>(minimum(tile_radius, 255.f));
                command.obscure = static_cast<u8>(obscure);
                if (const auto* attachment = item_attachment(s, item)) {
                    const auto format = message_rich_attachment_format(store, *attachment);
                    command.id = attachment->id;
                    command.kind = MessageMediaCommandKind::attachment;
                    command.format = static_cast<u8>(format);
                    command.ordinary_video =
                        format == MediaAttachmentFormat::mp4 && obscure == MessageMediaObscure::none;
                } else if (const auto* embed = item_embed(s, item)) {
                    MessageRichEmbedMediaSelection selected{};
                    if (message_rich_embed_resource_select(store, *embed, item.resource, &selected)) {
                        command.id = item.message_id;
                        command.kind = MessageMediaCommandKind::embed;
                        command.format = static_cast<u8>(selected.format);
                        command.animated = selected.animated && obscure == MessageMediaObscure::none;
                        command.embed_index = static_cast<u8>(item.rich_index);
                        command.embed_resource = static_cast<u8>(item.resource);
                        command.ordinary_video = selected.format == MediaAttachmentFormat::mp4 &&
                                                 embed->type != GatewayMessageEmbedType::gifv &&
                                                 obscure == MessageMediaObscure::none;
                    }
                }
                if (command.id && record)
                    media->storage[media->count++] = command;
            }
            // Media quads draw above this paint, so the ring sits in the gap around the tile.
            if ((hover || focus) &&
                !paint_stream_push_rounded_border(
                    out, screen.left - 2, screen.top - 2, screen.right + 2, screen.bottom + 2, Color{},
                    focus ? theme.tokens.border.focus : theme.interactive, tile_radius + 2, 2))
                return false;
            continue;
        }
        if (!ui_background_paint_surface(out, screen, theme, hover ? theme.surface_high : theme.surface,
                                         focus ? theme.tokens.border.focus : theme.surface_high, theme.scale.radius_md, 1, 1,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::low))
            return false;
        const UiRect icon_box{screen.left + 12, screen.top + 12, screen.left + 44, screen.top + 44};
        if (!paint_stream_push_rounded(out, icon_box.left, icon_box.top, icon_box.right, icon_box.bottom,
                                       theme.surface_high, 6) ||
            !ui_icon_paint(out, item_icon(s, item),
                           {icon_box.left + 6, icon_box.top + 6, icon_box.right - 6, icon_box.bottom - 6},
                           s.layout == UiSearchLayout::links ? theme.tokens.emphasis.outline : theme.text_secondary))
            return false;
        if (!p.secondary[v].generation)
            continue;
        const auto* node = ui_widget_manager_layout_node(s.widgets, p.secondary[v]);
        if (!node)
            continue;
        const auto r =
            s.layout == UiSearchLayout::files ? item_save_rectangle(screen) : item_jump_rectangle(screen);
        const bool active = hovered_widget(s, p.secondary[v]) || focused_widget(s, p.secondary[v]);
        if (!paint_stream_push_rounded(out, r.left, r.top, r.right, r.bottom,
                                       active
                                           ? ui_background_hover_color(theme, theme.interactive_hover, .28f)
                                           : theme.surface_high,
                                       6))
            return false;
        if (s.layout == UiSearchLayout::files &&
            !ui_icon_paint(out, UiIcon::download, {r.left + 6, r.top + 6, r.right - 6, r.bottom - 6},
                           active ? theme.text_primary : theme.text_secondary))
            return false;
    }
    return true;
}
bool paint_tabs(const UiSearchScene& s, const Theme& theme, PaintStream* out) noexcept {
    if (!s.content->tabs)
        return true;
    const auto p = s.panel;
    if (!paint_stream_push(out, p.left + 8, p.top + 115, p.right - 8, p.top + 116, theme.surface_high))
        return false;
    for (u32 i = 0; i < 5; ++i) {
        const auto r = bounds(s, s.tabs[i]);
        if (!ui_rect_has_area(r))
            continue;
        if ((hovered_widget(s, s.tabs[i]) || focused_widget(s, s.tabs[i])) &&
            !paint_stream_push_rounded(out, r.left, r.top, r.right, r.bottom - 3,
                                       ui_background_hover_color(theme, theme.interactive_hover, .18f), 6))
            return false;
        if (s.content->tab == i && !paint_stream_push_rounded(out, r.left + 8, r.bottom - 3, r.right - 8,
                                                              r.bottom, theme.tokens.emphasis.outline, 1.5f))
            return false;
    }
    return true;
}
bool rebuild(UiSearchScene& s) noexcept {
    const auto old_focus = s.widgets.focused;
    const UiSearchTarget focus = old_focus.generation && old_focus.slot < s.storage.widgets.count
                                     ? s.targets[old_focus.slot]
                                     : UiSearchTarget{};
    ui_widget_manager_reset(&s.widgets);
    ZeroMemory(s.targets, s.storage.widgets.count * sizeof(UiSearchTarget));
    auto root_style = ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_allow_overlap |
                                           ui_layout_node_clip_children);
    const auto root =
        ui_widget_manager_add_prevalidated(&s.widgets, ui_widget_invalid_handle(),
                                           {1,
                                            UiWidgetKind::container,
                                            ui_widget_none,
                                            {utf8_literal("Message search"), root_style, {}, 0, 1}});
    if (!root.ok())
        return false;
    s.root = root.handle;
    s.edit = add(s, s.field, {UiSearchAction::edit, 0, 0}, utf8_literal("Search messages"));
    // After the edit: the chip wins hit tests over the field it sits in.
    s.scope = ui_rect_has_area(s.scope_chip)
                  ? add(s, s.scope_chip, {UiSearchAction::scope, 0, 0}, utf8_literal("Remove search scope"))
                  : UiWidgetHandle{};
    const UiRect submit{s.field.right - 32, s.field.top, s.field.right, s.field.bottom};
    s.buttons[0] = add(s, submit, {UiSearchAction::submit, 0, 0}, utf8_literal("Search"));
    s.count = 0;
    s.first = 0;
    s.page_count = 0;
    if (s.content->panel) {
        const auto p = s.panel;
        // Icon button (× with a "Close search" tooltip), a 32-DIP square like the header buttons.
        s.buttons[1] = add(s, ui_search_close_rectangle(p), {UiSearchAction::close, 0, 0},
                           utf8_literal("Close search"));
        s.buttons[2] = add(s, {p.right - 196, p.top + 40, p.right - 124, p.top + 72},
                           {UiSearchAction::filters, 0, 0}, label(UiSearchAction::filters));
        s.buttons[8] = add(s, {p.right - 120, p.top + 40, p.right - 8, p.top + 72},
                           {UiSearchAction::sort, 0, 0}, label(UiSearchAction::sort));
        for (u32 i = 0; i < 5; ++i) {
            s.tabs[i] = {};
            if (!s.content->tabs)
                continue;
            const float width = (ui_rect_width(p) - 16) / 5;
            UiSearchTarget target{UiSearchAction::tab, 0, 0};
            target.source_index = i;
            s.tabs[i] =
                add(s, {p.left + 8 + i * width, p.top + 76, p.left + 8 + (i + 1) * width, p.top + 116},
                    target, tab_label(i), !chooser_open(s) && !s.content->sort_menu);
        }
        const UiSearchAction sorts[] = {UiSearchAction::newest, UiSearchAction::oldest,
                                        UiSearchAction::relevance};
        for (u32 i = 0; i < 3; ++i)
            s.buttons[3 + i] = {};
        s.buttons[6] =
            add(s, {p.left + 8, p.bottom - 36, p.left + 76, p.bottom - 8}, {UiSearchAction::previous, 0, 0},
                label(UiSearchAction::previous), s.content->previous);
        s.buttons[7] = add(s, {p.right - 72, p.bottom - 36, p.right - 8, p.bottom - 8},
                           {UiSearchAction::next, 0, 0}, label(UiSearchAction::next), s.content->next);
        s.page_count = 0;
        const u32 page_size = s.content->page_size ? s.content->page_size : 25;
        const u64 total_pages = s.content->total / page_size + (s.content->total % page_size != 0);
        const u64 page = s.content->offset / page_size;
        const u64 start = total_pages > 5 ? minimum(page > 2 ? page - 2 : 0, total_pages - 5) : 0;
        const u32 slots = minimum(5u, static_cast<u32>(maximum(0.f, (ui_rect_width(p) - 176) / 32)));
        const u32 visible_pages = minimum(slots, static_cast<u32>(minimum(total_pages, u64{5})));
        const float pages_left = (p.left + p.right - visible_pages * 32.f + 4.f) * .5f;
        for (u32 i = 0; i < slots && start + i < total_pages; ++i) {
            const u64 index = start + i;
            if (index > ~u32{0} / page_size)
                break;
            const u8 bytes = static_cast<u8>(decimal_write(s.page_text[i], index + 1));
            s.page_bytes[i] = bytes;
            const float x = pages_left + i * 32;
            s.pages[i] = add(s, {x, p.bottom - 36, x + 28, p.bottom - 8},
                             {UiSearchAction::page, 0, 0, static_cast<u32>(index * page_size)},
                             {s.page_text[i], bytes}, s.content->phase == UiSearchPhase::ready);
            ++s.page_count;
        }
        if (tiled(s)) {
            if (!add_items(s))
                return false;
        } else if (s.source && s.source->count) {
            while (s.first < s.source->count && s.row_offsets[s.first + 1] <= s.scroll_dips)
                ++s.first;
            for (u32 i = s.first; i < s.source->count && s.count < kUiSearchVisibleRows; ++i) {
                const float y = s.body.top + s.row_offsets[i] - s.scroll_dips;
                if (y >= s.body.bottom)
                    break;
                UiSearchRow row{};
                if (!s.source->resolve(s.source->context, i, &row) || !row.channel_id || !row.message_id ||
                    !utf8_validate(row.body).valid || !utf8_validate(row.author).valid ||
                    !utf8_validate(row.channel).valid || !utf8_validate(row.detail).valid)
                    return false;
                s.visible[s.count] = row;
                const bool rich = rich_enabled(s);
                const float group = rich ? 0.f : (row.group_first || !i) ? 28.f : 0.f;
                // Keep the shared 12-DIP scrollbar gutter outside the cards.
                const UiRect card{s.body.left + 8, y + group, s.body.right - 20,
                                  s.body.top + s.row_offsets[i + 1] - s.scroll_dips - 8};
                // Rich cards head with their channel, aligned to the avatar column,
                // and Jump at the trailing edge of the same row.
                const float heading = kDefaultMessageResultCardStyle.heading_height;
                const UiRect jump{card.right - 72, card.top + 4, card.right - 8, card.top + heading - 4};
                s.group_rectangles[s.count] =
                    rich ? UiRect{card.left + 16, y, maximum(card.left + 16, jump.left - 8), y + heading}
                         : UiRect{s.body.left + 12, y, s.body.right - 20, y + group};
                s.row_rectangles[s.count] = card;
                const auto r = ui_rect_intersection({card.left, y, card.right, card.bottom}, s.body);
                if (!ui_rect_has_area(r))
                    continue;
                UiSearchTarget target{UiSearchAction::result, row.channel_id, row.message_id};
                target.source_revision = s.source_revision;
                target.source_index = i;
                s.rows[s.count] = add(
                    s, rich ? ui_rect_intersection(jump, s.body) : r, target,
                    rich ? (row.access_check ? utf8_literal("Check access") : utf8_literal("Jump"))
                         : row.author,
                    (!row.unavailable || row.access_check) && !chooser_open(s) && !s.content->sort_menu);
                if (rich && !row.unavailable) {
                    const auto& native = s.rich_projection->scene;
                    const auto* outer = message_scene_find_row(native, i);
                    const MessageSceneRowWidget* projected[]{
                        outer, outer ? message_scene_snapshot_row(native, *outer) : nullptr};
                    for (u32 child = 0; child < 2; ++child) {
                        const auto* rich_row = projected[child];
                        const auto* record = !rich_row ? nullptr
                                             : child
                                                 ? message_rich_store_snapshot(*native.timeline->rich_store,
                                                                               rich_row->message_id)
                                                 : message_rich_store_find(*native.timeline->rich_store,
                                                                           rich_row->message_id);
                        if (!record)
                            continue;
                        for (u32 item = 0; item < record->attachment_count; ++item) {
                            const auto* node = message_scene_rich_item_layout_node(
                                native, *rich_row, MessageSceneRichItemKind::attachment, item);
                            float width = 0, height = 0;
                            if (!node || !record->attachments[item].url.bytes ||
                                message_rich_attachment_preview_extent(
                                    *native.timeline->rich_store, record->attachments[item],
                                    ui_rect_width(node->content_rectangle), &width, &height))
                                continue;
                            UiRect save_rectangle{};
                            if (message_rich_attachment_audio_supported(*native.timeline->rich_store,
                                                                        record->attachments[item])) {
                                UiAudioCardGeometry geometry{};
                                if (!ui_audio_card_geometry(node->content_rectangle,
                                                            ui_theme_audio_style(native.theme_styles),
                                                            &geometry, message_rich_record_voice(*record)))
                                    continue;
                                save_rectangle = geometry.download;
                            } else {
                                const UiRect file{
                                    node->rectangle.left, node->rectangle.top,
                                    minimum(node->rectangle.right, node->rectangle.left + 420),
                                    node->rectangle.top + 36};
                                save_rectangle = ui_attachment_save_rectangle(file, true);
                            }
                            const auto save = rich_item_clip(s, save_rectangle, node->clip_rectangle);
                            if (!ui_rect_has_area(save))
                                continue;
                            UiSearchTarget save_target{UiSearchAction::rich_item, row.channel_id,
                                                       row.message_id};
                            save_target.source_revision = s.source_revision;
                            save_target.source_index = i;
                            save_target.rich_index = static_cast<u16>(item);
                            save_target.snapshot = child != 0;
                            save_target.save = true;
                            if (!add(s, save, save_target, utf8_literal("Save attachment"),
                                     !chooser_open(s) && !s.content->sort_menu)
                                     .generation)
                                return false;
                        }
                    }
                }
                ++s.count;
            }
        }
        if (s.content->sort_menu)
            for (u32 i = 0; i < 3; ++i) {
                const float top = s.sort_chooser.top + 4 + i * 40;
                const float bottom = minimum(top + 40, s.sort_chooser.bottom - 4);
                if (bottom <= top)
                    break;
                s.buttons[3 + i] = add(s, {s.sort_chooser.left + 4, top, s.sort_chooser.right - 4, bottom},
                                       {sorts[i], 0, 0}, label(sorts[i]));
            }
    }
    if (s.choice_source) {
        const auto region = choice_rows_rectangle(s);
        const float row_height = choice_row_height(s);
        const float gutter = s.choice_count < s.choice_total ? 12.f : 0.f;
        for (u32 slot = 0; slot < s.choice_count; ++slot) {
            const u32 index = s.choice_first + slot;
            const float top = region.top + slot * row_height;
            const UiRect rectangle{region.left, top, region.right - gutter, top + row_height};
            UiSearchTarget target{UiSearchAction::filter_value, 0, 0};
            target.source_index = index;
            target.source_revision = s.choice_source_revision;
            const auto added = add(s, rectangle, target, s.choice_source->rows[index].label);
            if (!added.generation)
                return false;
            // Selection belongs to the focused native edit. Value rows accept
            // pointers without joining Tab traversal or taking the TSF target.
            s.widgets.storage[added.slot].flags &= static_cast<u16>(~ui_widget_focusable);
        }
    } else {
        if (chooser_open(s))
            for (u32 i = 0; i < (s.content->filters ? 12u : 5u); ++i) {
                const auto action =
                    !s.content->filters && i == 4
                        ? UiSearchAction::more_filters
                        : static_cast<UiSearchAction>(static_cast<u8>(UiSearchAction::filter_from) + i);
                const float row = s.content->filters ? 40.f : 48.f;
                const float top = s.chooser.top + 28 + i * row,
                            bottom = minimum(top + row, s.chooser.bottom - 4);
                if (bottom <= top)
                    break;
                add(s, {s.chooser.left + 4, top, s.chooser.right - 4, bottom}, {action, 0, 0},
                    label(action));
            }
        if (chooser_open(s) && recent_shown(s)) {
            const float heading = recent_top(s);
            if (heading + 28 <= s.chooser.bottom - 4 &&
                !add(s, {s.chooser.right - 68, heading, s.chooser.right - 8, heading + 28},
                     {UiSearchAction::clear_recent, 0, 0}, label(UiSearchAction::clear_recent))
                     .generation)
                return false;
            for (u32 i = 0; i < s.content->recent_count; ++i) {
                const float top = heading + 28 + i * 36, bottom = minimum(top + 36, s.chooser.bottom - 4);
                if (bottom <= top)
                    break;
                UiSearchTarget target{UiSearchAction::recent, 0, 0};
                target.source_index = i;
                if (!add(s, {s.chooser.left + 4, top, s.chooser.right - 4, bottom}, target,
                         s.content->recent[i])
                         .generation)
                    return false;
            }
        }
    }
    if (!ui_widget_manager_layout_rectangle(&s.widgets, widget_viewport(s)).ok())
        return false;
    for (u32 i = 1; i < s.widgets.count; ++i)
        if (ui_search_target_equal(s.targets[i], focus))
            ui_widget_manager_focus(&s.widgets, {static_cast<u16>(i), s.widgets.generation});
    count_label(s);
    ++s.revision;
    return true;
}
bool text(UiTextOutput* out, const UiSearchScene& s, UiWidgetHandle owner, Utf8View value, const UiRect& r,
          const Color& color, u32 flags = 0, UiTextWrapping wrap = UiTextWrapping::no_wrap,
          UiTextAlignment alignment = UiTextAlignment::start, bool chooser_text = false) noexcept {
    if (!value.bytes || !ui_rect_has_area(r))
        return true;
    const bool row = owner.generation && owner.slot < s.storage.widgets.count &&
                     (s.targets[owner.slot].action == UiSearchAction::result ||
                      s.targets[owner.slot].action == UiSearchAction::rich_item);
    const auto action = owner.generation && owner.slot < s.storage.widgets.count
                            ? s.targets[owner.slot].action
                            : UiSearchAction::none;
    const bool filter = chooser_text || action == UiSearchAction::filter_value ||
                        ui_search_action_filter(action) || action == UiSearchAction::more_filters ||
                        action == UiSearchAction::clear_recent || action == UiSearchAction::recent ||
                        (owner.slot == s.root.slot && utf8_equal(value, utf8_literal("Filters")));
    const bool sort = action == UiSearchAction::newest || action == UiSearchAction::oldest ||
                      action == UiSearchAction::relevance;
    if (!row && !filter && chooser_open(s) && ui_rect_has_area(ui_rect_intersection(r, s.chooser)))
        return true;
    if (!row && !sort && s.content->sort_menu && ui_rect_has_area(ui_rect_intersection(r, s.sort_chooser)))
        return true;
    auto region = row ? s.body : widget_viewport(s);
    if (chooser_text || action == UiSearchAction::filter_value || action == UiSearchAction::clear_recent ||
        action == UiSearchAction::recent)
        region = ui_rect_intersection(region, s.chooser);
    if (row && chooser_open(s) && ui_rect_has_area(ui_rect_intersection(r, s.chooser)))
        region.top = maximum(region.top, s.chooser.bottom);
    if (row && s.content->sort_menu && ui_rect_has_area(ui_rect_intersection(r, s.sort_chooser))) {
        // The right-aligned Sort menu covers timestamps, not the author to its
        // left. Preserve the visible left strip rather than hiding the row.
        if (r.left < s.sort_chooser.left)
            region.right = minimum(region.right, s.sort_chooser.left);
        else
            region.top = maximum(region.top, s.sort_chooser.bottom);
    }
    const auto clip = ui_rect_intersection(r, region);
    if (!ui_rect_has_area(clip))
        return true;
    return ui_text_output_push(
        out, {value,
              {&s.widgets, owner},
              r,
              clip,
              color,
              flags,
              alignment,
              wrap == UiTextWrapping::wrap ? UiTextAlignment::start : UiTextAlignment::center,
              wrap,
              UiTextOverflow::ellipsis,
              UiTextFace::normal,
              {},
              {}});
}
} // namespace
} // namespace maicord
namespace maicord {
float ui_search_scene_scroll_maximum(const UiSearchScene& s) noexcept {
    const float total = tiled(s) ? s.content_height : s.row_offsets[s.measured_count];
    return s.source ? maximum(0.f, total - ui_rect_height(s.body)) : 0;
}
u32 ui_search_scene_visible_tile_count(const UiSearchScene& s) noexcept {
    return current(s) && tiled(s) && s.layout == UiSearchLayout::grid ? s.rich_projection->visible_count
                                                                      : 0;
}
bool ui_search_scene_visible_tile(const UiSearchScene& s, u32 slot, UiSearchTarget* target) noexcept {
    if (!target || slot >= ui_search_scene_visible_tile_count(s))
        return false;
    const auto& item = s.rich_projection->items[s.rich_projection->visible[slot]];
    *target = item_target(s, item, true);
    return target->action == UiSearchAction::rich_item && ui_search_scene_target_current(s, *target);
}
bool ui_search_scene_current(const UiSearchScene& scene) noexcept {
    return current(scene);
}
bool ui_search_scene_bind_theme_styles(UiSearchScene* scene, const UiResolvedThemeStyles* styles) noexcept {
    if (!scene)
        return false;
    const bool changed = scene->theme_styles != styles;
    scene->theme_styles = styles;
    if (changed)
        scene->choice_geometry_revision = ~(styles && styles->ready ? styles->geometry_revision : 0u);
    const bool geometry =
        scene->rich_projection && message_scene_bind_theme_styles(&scene->rich_projection->scene, styles);
    return changed || geometry;
}
bool ui_search_scene_set_media_policy(UiSearchScene* scene, const MessageMediaPolicy& policy) noexcept {
    if (!scene ||
        (scene->media_policy.context == policy.context && scene->media_policy.query == policy.query &&
         scene->media_policy.revision == policy.revision))
        return false;
    scene->media_policy = policy;
    scene->media_policy_dirty = true;
    if (scene->rich_projection)
        message_scene_set_media_policy(&scene->rich_projection->scene, policy);
    return true;
}
MessageScene* ui_search_scene_message_scene(UiSearchScene* scene) noexcept {
    return scene ? const_cast<MessageScene*>(ui_search_scene_message_scene(*scene)) : nullptr;
}
const MessageScene* ui_search_scene_message_scene(const UiSearchScene& s) noexcept {
    if (!current(s) || !rich_enabled(s) || !s.rich_projection ||
        s.rich_projection->timeline != s.source->rich_timeline ||
        s.rich_projection->source_revision != s.source_revision)
        return nullptr;
    return &s.rich_projection->scene;
}
bool ui_search_scene_refresh_rich(UiSearchScene* s) noexcept {
    if (!s || !current(*s))
        return false;
    auto* scene = ui_search_scene_message_scene(s);
    if (!scene)
        return !rich_enabled(*s);
    auto& p = *s->rich_projection;
    if (p.height_updates == scene->timeline->list->height_updates &&
        p.layout_passes == scene->layout_passes && !scene->dirty)
        return true;
    if (!message_scene_update(scene, rich_viewport(*s)).ok())
        return false;
    configure_rich_input(*scene, *s->source);
    if (s->layout == UiSearchLayout::list)
        s->scroll_dips = ui_extent_dips(scene->timeline->scroll_units);
    if (!measure_rows(*s) || !rebuild(*s))
        return false;
    p.height_updates = scene->timeline->list->height_updates;
    p.layout_passes = scene->layout_passes;
    return true;
}
bool ui_search_scene_target_current(const UiSearchScene& s, UiSearchTarget target) noexcept {
    if (!current(s) || target.action == UiSearchAction::none)
        return false;
    if (target.action == UiSearchAction::filter_value)
        return s.choice_source && choices_current(s) &&
               target.source_revision == s.choice_source_revision &&
               target.source_index >= s.choice_first &&
               target.source_index < u32{s.choice_first} + s.choice_count;
    if (target.action != UiSearchAction::result && target.action != UiSearchAction::rich_item)
        return true;
    if (!s.source || target.source_revision != s.source_revision || target.source_index >= s.source->count)
        return false;
    UiSearchRow row{};
    if (!s.source->resolve(s.source->context, target.source_index, &row) ||
        (row.unavailable && !(target.action == UiSearchAction::result && row.access_check)) ||
        row.channel_id != target.channel_id || row.message_id != target.message_id)
        return false;
    if (target.action != UiSearchAction::rich_item)
        return true;
    const auto* native = ui_search_scene_message_scene(s);
    if (target.rich_kind == MessageSceneRichItemKind::poll) {
        const auto* poll_row = native ? message_scene_find_row(*native, target.source_index) : nullptr;
        const auto* record =
            poll_row ? message_store_resolve(*native->timeline->store, poll_row->message) : nullptr;
        const auto* poll = record && native->polls && native->polls->store
                               ? message_poll_store_find(*native->polls->store, record->id)
                               : nullptr;
        return !target.snapshot && !target.rich_index && !target.save &&
               target.resource == MediaEmbedResource::none && poll_row && !poll_row->redacted && record &&
               record->id == target.message_id && record->channel_id == target.channel_id && poll &&
               poll->channel_id == record->channel_id &&
               poll->content_revision == poll_row->poll_content_revision &&
               ui_widget_manager_input_enabled(native->widgets, poll_row->poll);
    }
    if (!native || !native->timeline->rich_store)
        return false;
    const auto& store = *native->timeline->rich_store;
    const auto* record = message_store_resolve(*native->timeline->store,
                                               message_scene_source_handle(*native, target.source_index));
    const auto* rich = target.snapshot ? message_rich_store_snapshot(store, target.message_id)
                                       : message_rich_store_find(store, target.message_id);
    if (!record || !rich || record->revision != rich->message_revision ||
        (target.save && target.rich_kind != MessageSceneRichItemKind::attachment) ||
        (target.resource != MediaEmbedResource::none &&
         target.rich_kind != MessageSceneRichItemKind::embed))
        return false;
    switch (target.rich_kind) {
    case MessageSceneRichItemKind::attachment:
        return target.rich_index < rich->attachment_count && rich->attachments[target.rich_index].url.bytes;
    case MessageSceneRichItemKind::embed: {
        if (target.rich_index >= rich->embed_count)
            return false;
        if (target.resource == MediaEmbedResource::none)
            return true;
        MessageRichEmbedMediaSelection selected{};
        return message_rich_embed_resource_select(store, rich->embeds[target.rich_index], target.resource,
                                                  &selected);
    }
    case MessageSceneRichItemKind::sticker:
        return target.rich_index < rich->sticker_count;
    case MessageSceneRichItemKind::component:
        return target.rich_index < rich->component_count && !rich->components[target.rich_index].disabled;
    default:
        return false;
    }
}
UiSearchTarget ui_search_scene_rich_widget_target(const UiSearchScene& s, UiWidgetHandle handle) noexcept {
    const auto* scene = ui_search_scene_message_scene(s);
    const auto* outer = scene ? message_scene_resolve(*scene, handle) : nullptr;
    const auto* record = outer ? message_store_resolve(*scene->timeline->store, outer->message) : nullptr;
    if (!record || outer->redacted)
        return {};
    if (outer->poll.slot == handle.slot && outer->poll.generation == handle.generation) {
        UiSearchTarget target{UiSearchAction::rich_item, record->channel_id, record->id};
        target.source_revision = s.source_revision;
        target.source_index = outer->source_index;
        target.rich_kind = MessageSceneRichItemKind::poll;
        return ui_search_scene_target_current(s, target) ? target : UiSearchTarget{};
    }
    const MessageSceneRowWidget* rows[]{outer, message_scene_snapshot_row(*scene, *outer)};
    for (u32 child = 0; child < 2; ++child) {
        const auto* row = rows[child];
        if (!row)
            continue;
        const u32 counts[]{row->attachment_item_count, row->embed_item_count, row->sticker_item_count,
                           row->component_item_count, row->notice_item_count};
        for (u32 kind = 0; kind < 4; ++kind)
            for (u32 index = 0; index < counts[kind]; ++index) {
                const auto item = message_scene_rich_item_widget(
                    *row, static_cast<MessageSceneRichItemKind>(kind), index);
                if (item.slot != handle.slot || item.generation != handle.generation ||
                    !ui_widget_manager_input_enabled(scene->widgets, handle))
                    continue;
                UiSearchTarget target{UiSearchAction::rich_item, record->channel_id, record->id};
                target.source_revision = s.source_revision;
                target.source_index = outer->source_index;
                target.rich_kind = static_cast<MessageSceneRichItemKind>(kind);
                target.rich_index = static_cast<u16>(index);
                target.snapshot = child != 0;
                if (target.rich_kind == MessageSceneRichItemKind::embed) {
                    const auto& store = *scene->timeline->rich_store;
                    const auto* rich = child ? message_rich_store_snapshot(store, record->id)
                                             : message_rich_store_find(store, record->id);
                    MessageRichEmbedGallery gallery{};
                    if (rich && message_rich_embed_gallery(store, *rich, index, &gallery)) {
                        MessageRichEmbedMediaSelection selected{};
                        target.rich_index = gallery.members[0];
                        if (message_rich_embed_gallery_select(store, rich->embeds[target.rich_index], &selected))
                            target.resource = selected.resource;
                    } else if (rich && index < rich->embed_count && !rich->embeds[index].url.bytes) {
                        MessageRichEmbedMediaSelection selected{};
                        if (message_rich_embed_media_select(store, rich->embeds[index], &selected))
                            target.resource = selected.resource;
                        else if (message_rich_embed_resource_select(
                                     store, rich->embeds[index], MediaEmbedResource::thumbnail, &selected))
                            target.resource = MediaEmbedResource::thumbnail;
                    }
                }
                return target;
            }
    }
    return {};
}
UiRect ui_search_scene_content_clip(const UiSearchScene& s, UiRect rectangle) noexcept {
    // Item layouts hide the message projection, including its text and emoji.
    if (!current(s) || tiled(s))
        return {};
    auto region = s.body;
    if (chooser_open(s) && ui_rect_has_area(ui_rect_intersection(rectangle, s.chooser)))
        region.top = maximum(region.top, s.chooser.bottom);
    if (s.content->sort_menu && ui_rect_has_area(ui_rect_intersection(rectangle, s.sort_chooser))) {
        if (rectangle.left < s.sort_chooser.left)
            region.right = minimum(region.right, s.sort_chooser.left);
        else
            region.top = maximum(region.top, s.sort_chooser.bottom);
    }
    return ui_rect_intersection(rectangle, region);
}
namespace {
const MessageSceneRowWidget* rich_target_row(const UiSearchScene& s, UiSearchTarget target,
                                             const MessageScene** output_scene) noexcept {
    const auto* scene = ui_search_scene_message_scene(s);
    if (!scene || !ui_search_scene_target_current(s, target))
        return nullptr;
    const auto* outer = message_scene_find_row(*scene, target.source_index);
    const auto* record = outer ? message_store_resolve(*scene->timeline->store, outer->message) : nullptr;
    if (!record || record->id != target.message_id || record->channel_id != target.channel_id)
        return nullptr;
    if (output_scene)
        *output_scene = scene;
    return target.snapshot ? message_scene_snapshot_row(*scene, *outer) : outer;
}
UiRect rich_item_clip(const UiSearchScene& s, UiRect rectangle, UiRect node_clip) noexcept {
    return ui_rect_intersection(ui_search_scene_content_clip(s, rectangle), node_clip);
}
} // namespace
bool ui_search_scene_rich_preview_geometry(const UiSearchScene& s, UiSearchTarget target, UiRect* rectangle,
                                           UiRect* clip) noexcept {
    if (!rectangle || !clip || target.action != UiSearchAction::rich_item)
        return false;
    if (tiled(s)) {
        u32 slot = 0;
        const auto* item = current(s) ? target_item(s, target, &slot) : nullptr;
        if (!item || s.layout != UiSearchLayout::grid ||
            (item->kind == UiSearchItemKind::embed && target.resource != item->resource) ||
            !ui_search_scene_target_current(s, target))
            return false;
        *rectangle = tile_cover(s, *item, s.rich_projection->screens[slot]);
        *clip = overlay_clip(s, s.rich_projection->screens[slot]);
        return ui_rect_has_area(*clip);
    }
    const MessageScene* scene = nullptr;
    const auto* row = rich_target_row(s, target, &scene);
    const auto* node =
        row ? message_scene_rich_item_layout_node(*scene, *row, target.rich_kind, target.rich_index)
            : nullptr;
    if (!node)
        return false;
    const auto& store = *scene->timeline->rich_store;
    const auto* rich = target.snapshot ? message_rich_store_snapshot(store, target.message_id)
                                       : message_rich_store_find(store, target.message_id);
    if (!rich)
        return false;
    if (target.rich_kind == MessageSceneRichItemKind::attachment &&
        target.rich_index < rich->attachment_count) {
        const auto& attachment = rich->attachments[target.rich_index];
        if (message_rich_attachment_audio_supported(store, attachment)) {
            UiAudioCardGeometry geometry{};
            if (!ui_audio_card_geometry(node->content_rectangle, ui_theme_audio_style(scene->theme_styles),
                                        &geometry, message_rich_record_voice(*rich)))
                return false;
            *rectangle = geometry.bounds;
        } else {
            float width = 0, height = 0;
            if (!message_rich_attachment_preview_extent(
                    store, attachment, ui_rect_width(node->content_rectangle), &width, &height))
                return false;
            *rectangle = {node->rectangle.left, node->rectangle.top, node->rectangle.left + width,
                          node->rectangle.top + height};
        }
    } else if (target.rich_kind == MessageSceneRichItemKind::embed &&
               target.rich_index < rich->embed_count) {
        const u32 i = target.rich_index;
        if (target.resource == MediaEmbedResource::thumbnail &&
            message_rich_embed_plan(store, rich->embeds[i]).corner_thumbnail) {
            const float measured = row->rich_item_layout.embed_widths[i];
            const float right = minimum(node->rectangle.right,
                                        node->rectangle.left + (measured > 0 ? measured : kMessageEmbedMaximumWidth));
            float width = 0, height = 0;
            if (!message_rich_embed_thumbnail_extent(
                    store, rich->embeds[i],
                    maximum(1.f, right - node->rectangle.left - kMessageEmbedContentInset - kMessageEmbedEndInset),
                    &width, &height))
                return false;
            *rectangle = message_rich_embed_thumbnail_rect(
                {node->rectangle.left, node->rectangle.top, right, node->rectangle.bottom}, width, height,
                message_rich_embed_plan(store, rich->embeds[i]).components);
            *clip = rich_item_clip(s, *rectangle, node->clip_rectangle);
            return ui_rect_has_area(*clip);
        }
        MessageRichEmbedMediaSelection selected{};
        // A gallery image is a tile of its host embed's gallery.
        MessageRichEmbedGalleryPlace place{};
        if (message_rich_embed_gallery_place(store, *rich, i, &place)) {
            const auto* host = message_scene_rich_item_layout_node(*scene, *row, MessageSceneRichItemKind::embed,
                                                                   place.host);
            if (!host || !message_rich_embed_gallery_select(store, rich->embeds[i], &selected) ||
                (target.resource != MediaEmbedResource::none && target.resource != selected.resource))
                return false;
            *rectangle = message_rich_embed_gallery_tile(
                message_rich_embed_media_rect(row->rich_item_layout, place.host, host->content_rectangle,
                                              message_rich_embed_media_only(rich->embeds[place.host])),
                place.count, place.ordinal);
            *clip = rich_item_clip(s, *rectangle, host->clip_rectangle);
            return ui_rect_has_area(*clip);
        }
        if (!message_rich_embed_media_select(store, rich->embeds[i], &selected) ||
            (target.resource != MediaEmbedResource::none && target.resource != selected.resource))
            return false;
        const float width = row->rich_item_layout.embed_media_widths[i];
        const float height = row->rich_item_layout.embed_media_heights[i];
        if (width <= 0 || height <= 0)
            return false;
        const float left =
            node->content_rectangle.left + message_rich_embed_media_left(message_rich_embed_media_only(rich->embeds[i]));
        const float top = node->content_rectangle.top + row->rich_item_layout.embed_media_tops[i];
        *rectangle = {left, top, left + width, top + height};
    } else if (target.rich_kind == MessageSceneRichItemKind::sticker &&
               target.rich_index < rich->sticker_count) {
        const float extent = kDefaultMessageStickerLayoutMetrics.preview_extent;
        *rectangle = {node->rectangle.left, node->rectangle.top, node->rectangle.left + extent,
                      node->rectangle.top + extent};
    } else
        return false;
    *clip = rich_item_clip(s, *rectangle, node->clip_rectangle);
    return ui_rect_has_area(*clip);
}
bool ui_search_scene_update(UiSearchScene* s, const UiSearchSource* source, const UiSearchContent* content,
                            UiRect viewport, UiRect title, const UiShellHeaderGeometry* header) noexcept {
    if (s && content && !choices_authorized(content->filter_choices)) {
        s->active = false;
        return false;
    }
    bool recent_valid =
        content && content->recent_count <= 5 && (!content->recent_count || content->recent);
    for (u32 i = 0; recent_valid && i < content->recent_count; ++i)
        recent_valid = content->recent[i].bytes && content->recent[i].bytes <= 256 &&
                       utf8_validate(content->recent[i]).valid;
    if (!s || !content || !recent_valid || !content->edit.active || !ui_rect_has_area(viewport) ||
        ui_rect_width(title) < 120 || !utf8_validate(content->heading).valid ||
        !utf8_validate(content->placeholder).valid || !utf8_validate(content->filter_summary).valid ||
        !utf8_validate(content->scope_chip).valid ||
        source && (!source->resolve || source->count > 125 ||
                   (source->current && !source->current(source->context))))
        return false;
    const float width = minimum(280.f, maximum(120.f, ui_rect_width(title) - 96));
    const UiRect field =
        header ? header->search_field
               : UiRect{title.right - width - 48, title.top + 8, title.right - 48, title.top + 40};
    if (!ui_rect_has_area(field) || !ui_coordinate_valid(field.left) || !ui_coordinate_valid(field.top) ||
        !ui_coordinate_valid(field.right) || !ui_coordinate_valid(field.bottom) ||
        ui_rect_width(field) < 80)
        return false;
    const UiRect chip = content->scope_chip.bytes ? ui_search_scope_chip_rectangle(field) : UiRect{};
    const UiRect text_area{ui_rect_has_area(chip) ? chip.right + 6 : field.left + 8, field.top, field.right - 36,
                           field.bottom};
    const u16 layout = static_cast<u16>((content->panel ? 1 : 0) | (content->filters ? 2 : 0) |
                                        (content->previous ? 4 : 0) | (content->next ? 8 : 0) |
                                        (content->sort << 4) | (static_cast<u16>(content->phase) << 7));
    const auto item_layout = layout_for(source, *content);
    const bool recent = !content->filters && content->recent_count && !content->edit.text.bytes;
    const u16 menu_layout = layout | (content->suggestions ? 1u << 10 : 0) |
                            (content->sort_menu ? 1u << 11 : 0) | (content->tabs ? 1u << 12 : 0) |
                            (static_cast<u16>(item_layout) << 13) | (recent ? 1u << 15 : 0);
    const auto* choices = content->filter_choices;
    const u32 geometry_revision =
        s->theme_styles && s->theme_styles->ready ? s->theme_styles->geometry_revision : 0;
    const bool same_publication =
        s->choice_source == choices && s->choice_rows == (choices ? choices->rows.data : nullptr) &&
        s->choice_source_revision == (choices ? choices->revision : 0) &&
        s->choice_total == (choices ? choices->rows.count : 0) &&
        s->choice_selected == (choices ? choices->selected : kUiSearchFilterNoSelection);
    // Host-owned strings are immutable under this publication. Warm frames
    // recheck authority above, without rescanning any candidate text.
    if (!same_publication && !choices_valid(choices)) {
        s->active = false;
        return false;
    }
    const bool same_choices = same_publication && s->choice_geometry_revision == geometry_revision;
    if (!s->media_policy_dirty && same_choices && s->active && s->source == source &&
        s->content == content && s->source_revision == (source ? source->revision : 0) &&
        s->content_revision == content->revision && ui_rect_equal(s->viewport, viewport) &&
        ui_rect_equal(s->field, field) && ui_rect_equal(s->text, text_area))
        return true;
    if (!s->media_policy_dirty && same_choices && s->active && s->source == source &&
        s->source_revision == (source ? source->revision : 0) && s->content_layout == menu_layout &&
        ui_rect_equal(s->viewport, viewport) && ui_rect_equal(s->field, field) &&
        ui_rect_equal(s->text, text_area) && s->pagination_total == content->total &&
        s->pagination_offset == content->offset && s->pagination_page_size == content->page_size) {
        s->content = content;
        s->content_revision = content->revision;
        count_label(*s);
        ++s->revision;
        return true;
    }
    const u32 widget_capacity = source && source->rich_timeline ? kUiSearchWidgetCapacity : 48;
    if (!s->storage.arena.base || s->storage.widgets.count < widget_capacity) {
        const u16 generation = s->widgets.generation;
        ui_fixed_widget_storage_release(&s->storage);
        if (!ui_fixed_widget_storage_initialize(&s->storage, widget_capacity,
                                                widget_capacity * sizeof(UiSearchTarget),
                                                alignof(UiSearchTarget)) ||
            !ui_widget_manager_initialize(&s->widgets, s->storage.widgets, s->storage.layout_nodes))
            return false;
        s->targets = ui_fixed_widget_storage_sidecar<UiSearchTarget>(s->storage);
        if (generation)
            s->widgets.generation = generation;
    }
    s->viewport = viewport;
    s->field = field;
    s->text = text_area;
    s->scope_chip = chip;
    const float panel_width = ui_search_panel_width(ui_rect_width(viewport));
    s->panel = {viewport.right - panel_width, title.bottom, viewport.right, viewport.bottom};
    s->body = {s->panel.left, s->panel.top + (content->tabs ? 144.f : 108.f), s->panel.right,
               s->panel.bottom - 44};
    s->layout = item_layout;
    const float chooser_width = minimum(360.f, ui_rect_width(widget_viewport(*s)) - 16);
    const float chooser_right =
        minimum(viewport.right - 8, maximum(viewport.left + chooser_width + 8, field.right));
    s->chooser = {chooser_right - chooser_width, field.bottom + 8, chooser_right,
                  minimum(viewport.bottom - 8,
                          field.bottom + 8 + 32 + (content->filters ? 12 * 40 : 5 * 48) +
                              (!content->filters && content->recent_count && !content->edit.text.bytes
                                   ? 32 + content->recent_count * 36
                                   : 0))};
    const u16 prior_first = s->choice_source == choices ? s->choice_first : u16{};
    s->choice_source = choices;
    s->choice_rows = choices ? choices->rows.data : nullptr;
    s->choice_source_revision = choices ? choices->revision : 0;
    s->choice_geometry_revision = geometry_revision;
    s->choice_total = choices ? static_cast<u16>(choices->rows.count) : u16{};
    s->choice_selected = choices ? choices->selected : kUiSearchFilterNoSelection;
    s->choice_details = false;
    s->choice_first = 0;
    s->choice_count = 0;
    if (choices) {
        for (u32 index = 0; index < choices->rows.count; ++index)
            s->choice_details = s->choice_details || choices->rows[index].detail.bytes != 0;
        const auto& metrics = ui_theme_metrics(s->theme_styles);
        const float top = maximum(viewport.top + 8, field.bottom + 8);
        const float available = maximum(0.f, minimum(420.f, viewport.bottom - 8 - top));
        const float chrome = 2 * metrics.menu_padding + choice_heading_height(*s) + choice_hint_height(*s);
        const float row_height = choice_row_height(*s);
        s->choice_count = static_cast<u16>(
            minimum(choices->rows.count, static_cast<u32>(maximum(0.f, available - chrome) / row_height)));
        s->choice_first = minimum(prior_first, static_cast<u16>(s->choice_total - s->choice_count));
        if (s->choice_selected == kUiSearchFilterNoSelection)
            s->choice_first = 0;
        else if (s->choice_selected < s->choice_first)
            s->choice_first = s->choice_selected;
        else if (s->choice_count && s->choice_selected >= s->choice_first + s->choice_count)
            s->choice_first = static_cast<u16>(s->choice_selected + 1 - s->choice_count);
        s->chooser.top = top;
        s->chooser.bottom = top + minimum(available, chrome + s->choice_count * row_height);
    }
    s->sort_chooser = {s->panel.right - 192, s->panel.top + 76, s->panel.right - 8,
                       minimum(s->panel.bottom - 44, s->panel.top + 204)};
    s->overlay = ui_rect_width(viewport) < 680;
    s->source = source;
    s->content = content;
    s->source_revision = source ? source->revision : 0;
    s->content_revision = content->revision;
    s->content_layout = menu_layout;
    s->pagination_total = content->total;
    s->pagination_offset = content->offset;
    s->pagination_page_size = content->page_size;
    if (!bind_rich(*s) || !measure_rows(*s)) {
        s->active = false;
        return false;
    }
    s->scroll_dips = maximum(0.f, minimum(s->scroll_dips, ui_search_scene_scroll_maximum(*s)));
    s->active = true;
    s->media_policy_dirty = false;
    return rebuild(*s);
}
void ui_search_scene_release(UiSearchScene* s) noexcept {
    if (!s)
        return;
    ui_fixed_widget_storage_release(&s->storage);
    if (s->rich_projection) {
        message_scene_release(&s->rich_projection->scene);
        ui_widget_storage_release(&s->rich_projection->widgets);
    }
    arena_release(&s->rich_projection_storage);
    *s = {};
}
bool ui_search_scene_scroll(UiSearchScene* s, float offset) noexcept {
    if (!s || !current(*s) || !ui_coordinate_valid(offset))
        return false;
    const float next = maximum(0.f, minimum(offset, ui_search_scene_scroll_maximum(*s)));
    if (next == s->scroll_dips)
        return true;
    s->scroll_dips = next;
    if (!bind_rich(*s) || !measure_rows(*s))
        return false;
    return rebuild(*s);
}
bool ui_search_scene_filter_choice_geometry(const UiSearchScene& scene, u32 source_index, UiRect* rectangle,
                                            UiRect* clip) noexcept {
    if (!rectangle || !clip || !current(scene) || !scene.choice_source ||
        source_index < scene.choice_first || source_index >= u32{scene.choice_first} + scene.choice_count)
        return false;
    for (u32 slot = 1; slot < scene.widgets.count; ++slot) {
        const auto& target = scene.targets[slot];
        if (target.action != UiSearchAction::filter_value || target.source_index != source_index ||
            !ui_search_scene_target_current(scene, target))
            continue;
        const auto* node = ui_widget_manager_layout_node(
            scene.widgets, {static_cast<u16>(slot), scene.widgets.generation});
        if (!node)
            return false;
        *rectangle = node->rectangle;
        *clip = ui_rect_intersection(node->rectangle,
                                     ui_rect_intersection(node->clip_rectangle, scene.chooser));
        return ui_rect_has_area(*clip);
    }
    return false;
}
bool ui_search_scene_filter_choice_avatar(const UiSearchScene& scene, u32 source_index, UiRect* rectangle,
                                          UiRect* clip, UiWidgetHandle* owner) noexcept {
    UiRect row{}, row_clip{};
    UiMenuRowGeometry geometry{};
    if (!rectangle || !clip || !owner || !ui_search_scene_filter_choice_geometry(scene, source_index, &row, &row_clip) ||
        !choice_person(*scene.choice_source, scene.choice_source->rows[source_index]) ||
        !choice_menu_geometry(scene, row, &geometry))
        return false;
    for (u32 slot = 1; slot < scene.widgets.count; ++slot) {
        const auto& target = scene.targets[slot];
        if (target.action == UiSearchAction::filter_value && target.source_index == source_index &&
            ui_search_scene_target_current(scene, target)) {
            *owner = {static_cast<u16>(slot), scene.widgets.generation};
            *rectangle = choice_avatar_rectangle(geometry, row);
            *clip = ui_rect_intersection(*rectangle, row_clip);
            return ui_rect_has_area(*clip);
        }
    }
    return false;
}
UiSearchTarget ui_search_scene_hit_test(const UiSearchScene& s, float x, float y) noexcept {
    if (!current(s))
        return {};
    const auto hit = ui_widget_manager_hit_test(s.widgets, x, y);
    const auto target =
        hit.generation && hit.slot < s.storage.widgets.count ? s.targets[hit.slot] : UiSearchTarget{};
    if ((target.action == UiSearchAction::result || target.action == UiSearchAction::rich_item) &&
        ((chooser_open(s) && ui_rect_contains(s.chooser, x, y)) ||
         (s.content->sort_menu && ui_rect_contains(s.sort_chooser, x, y))))
        return {};
    if (target.action != UiSearchAction::none)
        return ui_search_scene_target_current(s, target) ? target : UiSearchTarget{};
    const auto* rich_scene = tiled(s) ? nullptr : ui_search_scene_message_scene(s);
    if (!rich_scene || !ui_rect_contains(s.body, x, y) ||
        (chooser_open(s) && ui_rect_contains(s.chooser, x, y)) ||
        (s.content->sort_menu && ui_rect_contains(s.sort_chooser, x, y)))
        return {};
    const auto rich_hit = message_scene_hit_test(*rich_scene, x, y);
    const auto* outer =
        rich_hit.message_id ? message_scene_find_row(*rich_scene, rich_hit.source_index) : nullptr;
    if (outer && outer->redacted)
        return {};
    const auto* record =
        outer ? message_store_resolve(*rich_scene->timeline->store, outer->message) : nullptr;
    if (!record)
        return {};
    if (outer->poll_content_revision) {
        const auto* node = message_scene_layout_node(*rich_scene, outer->poll);
        if (node && ui_rect_contains(rich_item_clip(s, node->rectangle, node->clip_rectangle), x, y)) {
            UiSearchTarget poll_target{UiSearchAction::rich_item, record->channel_id, record->id};
            poll_target.source_revision = s.source_revision;
            poll_target.source_index = outer->source_index;
            poll_target.rich_kind = MessageSceneRichItemKind::poll;
            return ui_search_scene_target_current(s, poll_target) ? poll_target : UiSearchTarget{};
        }
    }
    const MessageSceneRowWidget* projected[]{outer, message_scene_snapshot_row(*rich_scene, *outer)};
    for (u32 child = 0; child < 2; ++child) {
        const auto* row = projected[child];
        if (!row)
            continue;
        const u32 counts[]{row->attachment_item_count, row->embed_item_count, row->sticker_item_count,
                           row->component_item_count, row->notice_item_count};
        for (u32 kind = 0; kind < 5; ++kind)
            for (u32 index = 0; index < counts[kind]; ++index) {
                const auto item_kind = static_cast<MessageSceneRichItemKind>(kind);
                const auto item = message_scene_rich_item_widget(*row, item_kind, index);
                const auto* node = message_scene_layout_node(*rich_scene, item);
                if (!node ||
                    !ui_rect_contains(rich_item_clip(s, node->rectangle, node->clip_rectangle), x, y))
                    continue;
                const auto* rich =
                    child ? message_rich_store_snapshot(*rich_scene->timeline->rich_store, record->id)
                          : message_rich_store_find(*rich_scene->timeline->rich_store, record->id);
                if (!rich)
                    continue;
                bool actionable =
                    kind == 0 && index < rich->attachment_count && rich->attachments[index].url.bytes;
                if (kind == 1 && index < rich->embed_count) {
                    MessageRichEmbedMediaSelection selected{};
                    actionable = rich->embeds[index].url.bytes ||
                                 message_rich_embed_media_select(*rich_scene->timeline->rich_store,
                                                                 rich->embeds[index], &selected) ||
                                 message_rich_embed_resource_select(
                                     *rich_scene->timeline->rich_store, rich->embeds[index],
                                     MediaEmbedResource::thumbnail, &selected);
                } else if (kind == 2)
                    actionable = index < rich->sticker_count && rich->stickers[index].id;
                else if (kind == 3)
                    actionable = index < rich->component_count && rich->components[index].type == 2 &&
                                 !rich->components[index].disabled;
                if (!actionable)
                    continue;
                UiSearchTarget result{UiSearchAction::rich_item, record->channel_id, record->id};
                result.source_revision = s.source_revision;
                result.source_index = outer->source_index;
                result.rich_kind = item_kind;
                result.rich_index = static_cast<u16>(index);
                result.snapshot = child != 0;
                if (item_kind == MessageSceneRichItemKind::attachment &&
                    message_rich_attachment_audio_supported(*rich_scene->timeline->rich_store,
                                                            rich->attachments[index])) {
                    UiAudioCardGeometry geometry{};
                    if (!ui_audio_card_geometry(node->content_rectangle,
                                                ui_theme_audio_style(rich_scene->theme_styles), &geometry,
                                                message_rich_record_voice(*rich)))
                        return {};
                    if (ui_rect_contains(geometry.download, x, y))
                        result.save = true;
                    else if (!ui_rect_contains(geometry.play_pause, x, y))
                        return {};
                }
                if (item_kind == MessageSceneRichItemKind::embed) {
                    MessageRichEmbedGallery gallery{};
                    const auto& store = *rich_scene->timeline->rich_store;
                    if (message_rich_embed_gallery(store, *rich, index, &gallery)) {
                        const auto block = message_rich_embed_media_rect(
                            row->rich_item_layout, index, node->content_rectangle,
                            message_rich_embed_media_only(rich->embeds[index]));
                        for (u32 tile = 0; tile < gallery.count && tile < kMessageEmbedGalleryDrawn; ++tile) {
                            const auto cell = message_rich_embed_gallery_tile(block, gallery.count, tile);
                            if (!ui_rect_contains(rich_item_clip(s, cell, node->clip_rectangle), x, y))
                                continue;
                            MessageRichEmbedMediaSelection selected{};
                            result.rich_index = gallery.members[tile];
                            if (message_rich_embed_gallery_select(store, rich->embeds[result.rich_index], &selected))
                                result.resource = selected.resource;
                            return result;
                        }
                        if (ui_rect_contains(rich_item_clip(s, block, node->clip_rectangle), x, y))
                            return {};
                        return result;
                    }
                    UiRect preview{}, preview_clip{};
                    result.resource = MediaEmbedResource::thumbnail;
                    if (!ui_search_scene_rich_preview_geometry(s, result, &preview, &preview_clip) ||
                        !ui_rect_contains(preview_clip, x, y)) {
                        MessageRichEmbedMediaSelection selected{};
                        result.resource = message_rich_embed_media_select(*rich_scene->timeline->rich_store,
                                                                          rich->embeds[index], &selected)
                                              ? selected.resource
                                              : MediaEmbedResource::none;
                        if (result.resource != MediaEmbedResource::none &&
                            (!ui_search_scene_rich_preview_geometry(s, result, &preview, &preview_clip) ||
                             !ui_rect_contains(preview_clip, x, y)))
                            result.resource = MediaEmbedResource::none;
                    }
                }
                return result;
            }
    }
    return {};
}
bool ui_search_scene_contains(const UiSearchScene& s, float x, float y) noexcept {
    return current(s) &&
           (ui_rect_contains(s.field, x, y) || (s.content->panel && ui_rect_contains(s.panel, x, y)) ||
            (chooser_open(s) && ui_rect_contains(s.chooser, x, y)));
}
bool ui_search_scene_avatar(const UiSearchScene& s, u32 slot, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !current(s) || !s.content->panel || slot >= s.count ||
        slot >= kUiSearchVisibleRows || !ui_widget_manager_resolve(s.widgets, s.rows[slot]))
        return false;
    const auto r = s.row_rectangles[slot];
    if (const auto* scene = ui_search_scene_message_scene(s)) {
        const auto* row = message_scene_find_row(*scene, s.first + slot);
        const auto* avatar = row ? message_scene_layout_node(*scene, row->avatar) : nullptr;
        if (!avatar)
            return false;
        *rectangle = avatar->rectangle;
    } else
        *rectangle = {r.left + 12, r.top + 12, r.left + 44, r.top + 44};
    auto region = s.body;
    if (chooser_open(s) && ui_rect_has_area(ui_rect_intersection(*rectangle, s.chooser)))
        region.top = maximum(region.top, s.chooser.bottom);
    if (s.content->sort_menu && ui_rect_has_area(ui_rect_intersection(*rectangle, s.sort_chooser))) {
        if (rectangle->left < s.sort_chooser.left)
            region.right = minimum(region.right, s.sort_chooser.left);
        else
            region.top = maximum(region.top, s.sort_chooser.bottom);
    }
    *clip = ui_rect_intersection(*rectangle, region);
    return ui_rect_has_area(*clip);
}
bool ui_search_scene_paint(const UiSearchScene& s, const Theme& theme, PaintStream* out,
                           const UiEditGeometry* geometry, u32 dpi,
                           MessageMediaCommandStream* rich_media, const MessageTextPresentation* rich_text) noexcept {
    if (!current(s))
        return true;
    if (!ui_edit_field_paint(out, s.field, s.text, s.content->edit, geometry, theme.base_lowest, theme, dpi,
                             8))
        return false;
    if (!ui_icon_paint(out, UiIcon::search,
                       {s.field.right - 26, s.field.top + 6, s.field.right - 6, s.field.bottom - 6},
                       theme.text_secondary))
        return false;
    if (s.scope.generation) {
        // The scope reads as a selected pill (the emphasis tint of selected
        // tabs); its x removes it.
        const auto chip = bounds(s, s.scope);
        const bool focus = focused_widget(s, s.scope), hover = hovered_widget(s, s.scope);
        if (!ui_background_paint_surface(out, chip, theme, ui_emphasis_selected(theme),
                                         focus   ? theme.tokens.border.focus
                                         : hover ? theme.tokens.emphasis.outline
                                                 : Color{},
                                         theme.scale.radius_sm, focus || hover ? 1.f : 0.f, 1,
                                         UiBackgroundSurfaceMode::gradient,
                                         UiBackgroundTier::high) ||
            !ui_icon_paint(out, UiIcon::close, {chip.right - 18, chip.top + 6, chip.right - 6, chip.bottom - 6},
                           hover || focus ? theme.text_primary : theme.text_secondary))
            return false;
    }
    if (s.content->panel) {
        const auto p = s.panel;
        if (!ui_background_paint_surface(out, p, theme, theme.base_lower, {}, 0, 0, 1,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::chat))
            return false;
        if (!paint_tabs(s, theme, out) ||
            !paint_stream_push_clip(out, s.body.left, s.body.top, s.body.right, s.body.bottom))
            return false;
        bool rows_ok = !tiled(s) || paint_items(s, theme, out, rich_media);
        for (u32 i = 0; rows_ok && i < s.count; ++i) {
            const auto r = s.row_rectangles[i];
            if (!ui_background_paint_surface(
                    out, r, theme, s.visible[i].hit ? theme.surface : theme.base_lower,
                    s.visible[i].hit ? theme.surface_high : Color{}, 10, s.visible[i].hit ? 1.f : 0.f, 1,
                    UiBackgroundSurfaceMode::gradient, UiBackgroundTier::low))
                rows_ok = false;
            if (rich_enabled(s))
                continue;
            const float x = r.left + 28, y = r.top + 28;
            rows_ok = rows_ok &&
                      paint_stream_push_rounded(out, x - 16, y - 16, x + 16, y + 16,
                                                theme.avatar_placeholder, theme.scale.radius_full) &&
                      paint_stream_push_rounded(out, x - 5, y - 9, x + 5, y + 1, theme.text_secondary, 5) &&
                      paint_stream_push_rounded(out, x - 9, y + 3, x + 9, y + 11, theme.text_secondary, 7);
        }
        if (rows_ok && rich_enabled(s) && !tiled(s)) {
            const auto* scene = ui_search_scene_message_scene(s);
            MessagePresentationResult result{};
            rows_ok =
                scene && rich_media && message_presentation_build(*scene, theme, out, rich_media, &result, rich_text);
            if (rows_ok)
                for (u32 i = 0; i < rich_media->count; ++i) {
                    auto& command = rich_media->storage[i];
                    command.clip = rich_item_clip(s, command.rectangle, command.clip);
                }
        }
        if (rows_ok && rich_enabled(s) && !tiled(s))
            for (u32 i = 0; i < s.count; ++i) {
                if (!s.visible[i].unavailable)
                    continue;
                rows_ok = rows_ok && ui_background_paint_surface(out, s.row_rectangles[i], theme,
                                                                 theme.surface_high, theme.interactive,
                                                                 ui_theme_metrics(s.theme_styles).control_radius, 1, .9f, UiBackgroundSurfaceMode::gradient,
                                                                 UiBackgroundTier::high);
            }
        if (!paint_stream_pop_clip(out) || !rows_ok)
            return false;
        if (!paint_stream_push(out, p.left, p.bottom - 44, p.right, p.bottom - 43, theme.surface_high))
            return false;
    }
    if (s.content->sort_menu &&
        !ui_surface_paint(out, s.sort_chooser, theme, UiElevation::popover,
                          ui_theme_metrics(s.theme_styles).control_radius, &theme.surface_high))
        return false;
    if (chooser_open(s) &&
        !ui_surface_paint(out, s.chooser, theme, UiElevation::popover,
                          s.choice_source ? ui_theme_metrics(s.theme_styles).menu_radius
                                          : ui_theme_metrics(s.theme_styles).control_radius,
                          &theme.surface_high))
        return false;
    for (u32 i = 1; i < s.widgets.count; ++i) {
        const auto action = s.targets[i].action;
        if (action == UiSearchAction::edit || action == UiSearchAction::submit)
            continue;
        const auto r = bounds(s, {static_cast<u16>(i), s.widgets.generation});
        // Tabs and item widgets paint their own states.
        if (action == UiSearchAction::tab ||
            (tiled(s) && (action == UiSearchAction::result || action == UiSearchAction::rich_item)))
            continue;
        const bool selected =
            (action == UiSearchAction::filter_value && s.targets[i].source_index == s.choice_selected) ||
            (action == UiSearchAction::newest && s.content->sort == 0) ||
            (action == UiSearchAction::oldest && s.content->sort == 1) ||
            (action == UiSearchAction::relevance && s.content->sort == 2) ||
            (action == UiSearchAction::page &&
             s.targets[i].page_offset / (s.content->page_size ? s.content->page_size : 25) ==
                 s.content->offset / (s.content->page_size ? s.content->page_size : 25));
        const bool hovered =
                       s.widgets.hovered.generation == s.widgets.generation && s.widgets.hovered.slot == i,
                   captured = s.widgets.captured.generation == s.widgets.generation &&
                              s.widgets.captured.slot == i;
        const bool filter = action == UiSearchAction::filter_value || ui_search_action_filter(action) ||
                            action == UiSearchAction::more_filters ||
                            action == UiSearchAction::clear_recent || action == UiSearchAction::recent;
        const bool sort = action == UiSearchAction::newest || action == UiSearchAction::oldest ||
                          action == UiSearchAction::relevance;
        if ((!filter && chooser_open(s) && ui_rect_has_area(ui_rect_intersection(r, s.chooser))) ||
            (!sort && s.content->sort_menu && ui_rect_has_area(ui_rect_intersection(r, s.sort_chooser))))
            continue;
        // Jump buttons under an open menu stay hidden with the rest of their card.
        if (rich_enabled(s) && action == UiSearchAction::result &&
            !ui_background_paint_surface(out, r, theme, theme.surface_high, theme.interactive,
                                         ui_theme_metrics(s.theme_styles).control_radius, 1, .9f,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high))
            return false;
        if (action == UiSearchAction::rich_item && s.targets[i].save) {
            if (!ui_attachment_save_paint(out, {&s.widgets, {static_cast<u16>(i), s.widgets.generation}}, r,
                                          ui_search_scene_content_clip(s, r), theme, true))
                return false;
            continue;
        }
        if ((action == UiSearchAction::filters || action == UiSearchAction::sort) &&
            !ui_background_paint_surface(out, r, theme, theme.surface, theme.surface_high, theme.scale.radius_md, 1, .9f,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                                         UiBackgroundBorderTone::palette))
            return false;
        if ((hovered || captured) &&
            !paint_stream_push_rounded(
                out, r.left, r.top, r.right, r.bottom,
                ui_background_hover_color(theme, captured ? theme.interactive : theme.interactive_hover,
                                          captured ? .32f : .20f),
                action == UiSearchAction::filter_value ? ui_theme_metrics(s.theme_styles).compact_radius
                                                       : 4.f))
            return false;
        if (selected &&
            !paint_stream_push_rounded(out, r.left, r.top, r.right, r.bottom,
                                       ui_background_hover_color(theme, theme.interactive, .28f),
                                       action == UiSearchAction::filter_value
                                           ? ui_theme_metrics(s.theme_styles).compact_radius
                                           : 4.f))
            return false;
        if (action == UiSearchAction::close) {
            const float icon = ui_icon_button_extent(ui_rect_width(r) - 8);
            const float x = (r.left + r.right - icon) * .5f, y = (r.top + r.bottom - icon) * .5f;
            if (!ui_icon_paint(out, UiIcon::close, {x, y, x + icon, y + icon},
                               hovered || captured ? theme.text_primary : theme.text_secondary))
                return false;
            continue;
        }
        if (sort) {
            const float x = r.right - 16, y = (r.top + r.bottom) * .5f;
            if (!paint_stream_push_rounded_border(out, x - 8, y - 8, x + 8, y + 8, {}, theme.text_secondary,
                                                  theme.scale.radius_full, 1) ||
                (selected &&
                 !paint_stream_push_rounded(out, x - 4, y - 4, x + 4, y + 4, theme.tokens.emphasis.fill, theme.scale.radius_sm)))
                return false;
        }
        if (action == UiSearchAction::filter_value) {
            UiMenuRowGeometry choice_geometry{};
            if (!choice_menu_geometry(s, r, &choice_geometry) ||
                !paint_stream_push_clip(out, choice_geometry.clip.left, choice_geometry.clip.top,
                                        choice_geometry.clip.right, choice_geometry.clip.bottom))
                return false;
            const auto index = s.targets[i].source_index;
            const auto& choice = s.choice_source->rows[index];
            const UiRect avatar = choice_avatar_rectangle(choice_geometry, r);
            const bool icon =
                choice_person(*s.choice_source, choice)
                    ? paint_stream_push_rounded(out, avatar.left, avatar.top, avatar.right, avatar.bottom,
                                                theme.avatar_placeholder, ui_rect_height(avatar) * .5f)
                    : ui_icon_paint(out, choice_icon(*s.choice_source, choice), choice_geometry.icon,
                                    theme.text_secondary);
            if (!paint_stream_pop_clip(out) || !icon)
                return false;
            continue;
        }
        if (action == UiSearchAction::sort) {
            const float cy = (r.top + r.bottom) * .5f;
            if (!ui_icon_paint(out, UiIcon::chevron_down, {r.right - 24, cy - 8, r.right - 8, cy + 8},
                               theme.text_secondary))
                return false;
        }
        if (action == UiSearchAction::recent) {
            const float cy = (r.top + r.bottom) * .5f;
            if (!ui_icon_paint(out, UiIcon::recent, {r.left + 8, cy - 9, r.left + 26, cy + 9},
                               theme.text_secondary))
                return false;
            continue;
        }
        if (filter && action != UiSearchAction::clear_recent) {
            const UiIcon icons[] = {UiIcon::user,       UiIcon::channel,   UiIcon::attachment,
                                    UiIcon::mention,    UiIcon::calendar,  UiIcon::date_before,
                                    UiIcon::date_after, UiIcon::pin,       UiIcon::link,
                                    UiIcon::file,       UiIcon::file_text, UiIcon::bot};
            if (!paint_stream_push_clip(out, r.left, r.top, r.right, r.bottom))
                return false;
            const float cy = (r.top + r.bottom) * .5f;
            const bool icon_ok = ui_icon_paint(
                out,
                action == UiSearchAction::more_filters
                    ? UiIcon::filters
                    : icons[static_cast<u8>(action) - static_cast<u8>(UiSearchAction::filter_from)],
                {r.left + 8, cy - 10, r.left + 28, cy + 10}, theme.text_secondary);
            if (!paint_stream_pop_clip(out) || !icon_ok)
                return false;
        }
    }
    if (s.choice_source && s.choice_count && s.choice_count < s.choice_total &&
        !ui_scrollbar_paint(out,
                            ui_scrollbar_geometry(choice_rows_rectangle(s),
                                                  s.choice_first * choice_row_height(s),
                                                  s.choice_total * choice_row_height(s)),
                            theme))
        return false;
    if (s.content->panel &&
        !ui_scrollbar_paint(
            out,
            ui_scrollbar_geometry(s.body, s.scroll_dips,
                                  tiled(s) ? s.content_height : s.row_offsets[s.measured_count]),
            theme))
        return false;
    return true;
}
bool ui_search_scene_text_output(const UiSearchScene& s, const Theme& theme, UiTextOutput* out,
                                 const UiEditGeometry* edit) noexcept {
    if (!current(s))
        return true;
    if (!ui_edit_field_text_output(
            out, {&s.widgets, s.edit}, s.text, s.content->edit, edit,
            s.content->placeholder.bytes ? s.content->placeholder : utf8_literal("Search messages"), theme))
        return false;
    if (s.scope.generation) {
        const auto chip = bounds(s, s.scope);
        if (!text(out, s, s.scope, s.content->scope_chip, {chip.left + 8, chip.top, chip.right - 20, chip.bottom},
                  theme.text_primary, text_style_subtext))
            return false;
    }
    if (s.content->panel) {
        const auto p = s.panel;
        if (!text(out, s, s.root,
                  s.content->heading.bytes ? s.content->heading : utf8_literal("Search results"),
                  {p.left + 12, p.top + 8, p.right - 64, p.top + 32}, theme.text_primary,
                  text_style_bold) ||
            !text(out, s, s.root, {s.count_text, s.count_bytes},
                  {p.left + 12, p.top + 40, p.right - 200, p.top + 72}, theme.text_secondary,
                  text_style_subtext) ||
            (!chooser_open(s) &&
             !text(out, s, s.root,
                   s.content->filter_summary.bytes ? s.content->filter_summary : s.content->status,
                   s.content->tabs ? UiRect{p.left + 12, p.top + 118, p.right - 12, p.top + 142}
                                   : UiRect{p.left + 12, p.top + 76, p.right - 12, p.top + 100},
                   theme.text_secondary, text_style_subtext)))
            return false;
        if (tiled(s)) {
            const auto& projection = *s.rich_projection;
            for (u32 v = 0; v < projection.visible_count; ++v) {
                const auto& item = projection.items[projection.visible[v]];
                const auto r = projection.screens[v];
                const auto owner = projection.primary[v];
                if (s.layout == UiSearchLayout::grid) {
                    const MessageMediaObscure obscure = item_obscure(s, item);
                    if (obscure != MessageMediaObscure::none &&
                        !text(out, s, owner, message_media_obscure_label(obscure),
                              {r.left + 4, r.bottom - 28, r.right - 4, r.bottom - 6}, theme.text_secondary,
                              text_style_subtext, UiTextWrapping::no_wrap, UiTextAlignment::center))
                        return false;
                    continue;
                }
                const float right = projection.secondary[v].generation ? r.right - 72 : r.right - 12;
                const auto link = item_link(s, item);
                const auto detail = s.layout == UiSearchLayout::files
                                        ? Utf8View{projection.details[v], projection.detail_bytes[v]}
                                        : url_hostname(link);
                const auto title = item_title(s, item);
                if (!text(out, s, owner, title.bytes ? title : utf8_literal("Untitled"),
                          {r.left + 56, r.top + 9, right, r.top + 29}, theme.text_primary,
                          text_style_bold) ||
                    !text(out, s, owner, detail, {r.left + 56, r.top + 29, right, r.top + 47},
                          theme.text_secondary, text_style_subtext))
                    return false;
                if (s.layout == UiSearchLayout::links && projection.secondary[v].generation &&
                    !text(out, s, projection.secondary[v], utf8_literal("Jump"), item_jump_rectangle(r),
                          theme.text_primary, text_style_subtext, UiTextWrapping::no_wrap,
                          UiTextAlignment::center))
                    return false;
            }
            if (!projection.item_count && s.content->phase == UiSearchPhase::ready && !s.content->stale &&
                !text(out, s, s.root,
                      s.layout == UiSearchLayout::grid    ? utf8_literal("No media on this page.")
                      : s.layout == UiSearchLayout::files ? utf8_literal("No files on this page.")
                                                          : utf8_literal("No links on this page."),
                      {s.body.left + 12, s.body.top + 16, s.body.right - 12, s.body.top + 40},
                      theme.text_secondary, text_style_subtext, UiTextWrapping::no_wrap,
                      UiTextAlignment::center))
                return false;
        }
        for (u32 i = 0; s.content->tabs && i < 5; ++i) {
            const auto r = bounds(s, s.tabs[i]);
            if (!ui_rect_has_area(r))
                continue;
            const auto color = s.content->tab == i ? theme.text_primary : theme.text_secondary;
            const bool counted = s.tab_bytes[i] != 0;
            const float mid = (r.top + r.bottom - 3) * .5f;
            if (!text(out, s, s.tabs[i], tab_label(i),
                      counted ? UiRect{r.left + 2, r.top + 3, r.right - 2, mid + 2}
                              : UiRect{r.left + 2, r.top, r.right - 2, r.bottom - 3},
                      color,
                      s.content->tab == i ? text_style_bold | text_style_subtext : text_style_subtext,
                      UiTextWrapping::no_wrap, UiTextAlignment::center) ||
                (counted && !text(out, s, s.tabs[i], {s.tab_text[i], s.tab_bytes[i]},
                                  {r.left + 2, mid, r.right - 2, r.bottom - 5}, theme.text_secondary,
                                  text_style_subtext, UiTextWrapping::no_wrap, UiTextAlignment::center)))
                return false;
        }
        for (u32 i = 0; i < s.count; ++i) {
            const auto& row = s.visible[i];
            const auto r = s.row_rectangles[i];
            if (rich_enabled(s)) {
                if (row.unavailable) {
                    if (!text(out, s, s.root, row.body,
                              {r.left + 16, r.top + 12, r.right - (row.access_check ? 88 : 16), r.bottom - 12},
                              theme.text_secondary,
                              text_style_subtext))
                        return false;
                    if (row.access_check) {
                        const auto* check = ui_widget_manager_layout_node(s.widgets, s.rows[i]);
                        if (check && !text(out, s, s.rows[i], utf8_literal("Check access"),
                                           check->rectangle, theme.text_primary, text_style_subtext,
                                           UiTextWrapping::no_wrap, UiTextAlignment::center))
                            return false;
                    }
                    continue;
                }
                const auto* jump = ui_widget_manager_layout_node(s.widgets, s.rows[i]);
                if (!text(out, s, s.rows[i], row.channel, s.group_rectangles[i], theme.text_secondary,
                          text_style_subtext | text_style_bold) ||
                    (jump &&
                     !text(out, s, s.rows[i], utf8_literal("Jump"), jump->rectangle, theme.text_primary,
                           text_style_subtext, UiTextWrapping::no_wrap, UiTextAlignment::center)))
                    return false;
                continue;
            }
            if ((ui_rect_has_area(s.group_rectangles[i]) &&
                 !text(out, s, s.rows[i], row.channel, s.group_rectangles[i], theme.text_secondary,
                       text_style_subtext | text_style_bold)) ||
                !text(out, s, s.rows[i], row.author, plain_header(r, row).name,
                      row.hit ? theme.text_primary : theme.text_secondary, text_style_bold) ||
                !text(out, s, s.rows[i], row.detail, plain_header(r, row).timestamp,
                      theme.text_secondary, text_style_subtext) ||
                !text(out, s, s.rows[i], row.body, {r.left + 52, r.top + 36, r.right - 12, r.bottom - 12},
                      row.hit ? theme.text_primary : theme.text_secondary, 0, UiTextWrapping::wrap))
                return false;
        }
    }
    if (s.choice_source) {
        const auto& metrics = ui_theme_metrics(s.theme_styles);
        const auto heading =
            s.choice_source->heading.bytes ? s.choice_source->heading : utf8_literal("Search values");
        const auto hint = s.choice_source->hint.bytes
                              ? s.choice_source->hint
                              : (!s.choice_total ? utf8_literal("No matches. Keep typing.")
                                                 : Utf8View{});
        const float left = s.chooser.left + metrics.menu_padding,
                    right = s.chooser.right - metrics.menu_padding;
        const float top = s.chooser.top + metrics.menu_padding;
        if (!text(out, s, s.root, heading, {left, top, right, top + choice_heading_height(s)},
                  theme.text_primary, text_style_bold, UiTextWrapping::no_wrap, UiTextAlignment::start,
                  true))
            return false;
        const float bottom = s.chooser.bottom - metrics.menu_padding;
        if (!text(out, s, s.root, hint, {left, maximum(top, bottom - choice_hint_height(s)), right, bottom},
                  theme.text_secondary, text_style_subtext, UiTextWrapping::wrap, UiTextAlignment::start,
                  true))
            return false;
    } else {
        if (chooser_open(s) &&
            !text(out, s, s.root, utf8_literal("Filters"),
                  {s.chooser.left + 12, s.chooser.top + 4, s.chooser.right - 12, s.chooser.top + 26},
                  theme.text_secondary, text_style_subtext | text_style_bold))
            return false;
        if (chooser_open(s) && recent_shown(s) &&
            !text(out, s, s.root, utf8_literal("Recent searches"),
                  {s.chooser.left + 12, recent_top(s), s.chooser.right - 76, recent_top(s) + 28},
                  theme.text_secondary, text_style_subtext | text_style_bold, UiTextWrapping::no_wrap,
                  UiTextAlignment::start, true))
            return false;
    }
    for (u32 i = 1; i < s.widgets.count; ++i) {
        const auto a = s.targets[i].action;
        if (a == UiSearchAction::edit || a == UiSearchAction::result || a == UiSearchAction::submit ||
            a == UiSearchAction::rich_item || a == UiSearchAction::tab)
            continue;
        auto r = bounds(s, {static_cast<u16>(i), s.widgets.generation});
        r.left += 4;
        r.right -= 4;
        if (a == UiSearchAction::filter_value) {
            UiMenuRowGeometry geometry{};
            const auto rectangle = bounds(s, {static_cast<u16>(i), s.widgets.generation});
            if (!choice_menu_geometry(s, rectangle, &geometry))
                return false;
            const auto& choice = s.choice_source->rows[s.targets[i].source_index];
            const UiWidgetHandle owner{static_cast<u16>(i), s.widgets.generation};
            if (!text(out, s, owner, choice.label, geometry.primary, theme.text_primary, text_style_bold) ||
                !text(out, s, owner, choice.detail, geometry.detail, theme.text_secondary,
                      text_style_subtext))
                return false;
            continue;
        }
        if (a == UiSearchAction::page) {
            for (u32 page = 0; page < s.page_count; ++page)
                if (s.pages[page].slot == i &&
                    !text(out, s, s.pages[page], {s.page_text[page], s.page_bytes[page]}, r,
                          theme.text_primary, text_style_subtext, UiTextWrapping::no_wrap,
                          UiTextAlignment::center))
                    return false;
            continue;
        }
        if (ui_search_action_filter(a) || a == UiSearchAction::more_filters) {
            const Utf8View hints[] = {utf8_literal("from:"),
                                      utf8_literal("in:"),
                                      utf8_literal("has:image"),
                                      utf8_literal("mentions:"),
                                      utf8_literal("on:YYYY-MM-DD"),
                                      utf8_literal("before:YYYY-MM-DD"),
                                      utf8_literal("after:YYYY-MM-DD"),
                                      utf8_literal("pinned:true"),
                                      utf8_literal("linkFrom:youtube.com"),
                                      utf8_literal("fileType:pdf"),
                                      utf8_literal("fileName:report"),
                                      utf8_literal("authorType:bot")};
            r.left += 32;
            const auto hint =
                a == UiSearchAction::more_filters
                    ? utf8_literal("Dates, files, links and more")
                    : hints[static_cast<u8>(a) - static_cast<u8>(UiSearchAction::filter_from)];
            const float mid = (r.top + r.bottom) * .5f;
            if (!text(out, s, {static_cast<u16>(i), s.widgets.generation}, label(a),
                      {r.left, r.top + 2, r.right, mid + 2}, theme.text_primary, text_style_subtext) ||
                !text(out, s, {static_cast<u16>(i), s.widgets.generation}, hint,
                      {r.left, mid, r.right, r.bottom - 2}, theme.text_secondary, text_style_subtext))
                return false;
            continue;
        }
        if (a == UiSearchAction::recent || a == UiSearchAction::clear_recent) {
            const auto value =
                a == UiSearchAction::recent && s.targets[i].source_index < s.content->recent_count
                    ? s.content->recent[s.targets[i].source_index]
                    : label(a);
            if (!text(out, s, {static_cast<u16>(i), s.widgets.generation}, value,
                      a == UiSearchAction::recent ? UiRect{r.left + 28, r.top, r.right, r.bottom} : r,
                      a == UiSearchAction::recent ? theme.text_primary : theme.text_secondary,
                      text_style_subtext, UiTextWrapping::no_wrap,
                      a == UiSearchAction::recent ? UiTextAlignment::start : UiTextAlignment::center))
                return false;
            continue;
        }
        if (a == UiSearchAction::sort) {
            const Utf8View sorts[] = {label(UiSearchAction::newest), label(UiSearchAction::oldest),
                                      label(UiSearchAction::relevance)};
            if (!text(out, s, {static_cast<u16>(i), s.widgets.generation},
                      sorts[s.content->sort < 3 ? s.content->sort : 0],
                      {r.left + 4, r.top, r.right - 20, r.bottom}, theme.text_primary, text_style_subtext,
                      UiTextWrapping::no_wrap, UiTextAlignment::center))
                return false;
            continue;
        }
        if (a == UiSearchAction::close)
            continue;
        if (a == UiSearchAction::newest || a == UiSearchAction::oldest || a == UiSearchAction::relevance)
            r.right -= 28;
        const bool enabled = (s.widgets.storage[i].flags & ui_widget_enabled) != 0;
        if (!text(out, s, {static_cast<u16>(i), s.widgets.generation}, label(a), r,
                  enabled ? theme.text_primary : theme.text_secondary, text_style_subtext,
                  UiTextWrapping::no_wrap,
                  a == UiSearchAction::filters || a == UiSearchAction::sort ||
                          a == UiSearchAction::previous || a == UiSearchAction::next ||
                          a == UiSearchAction::close
                      ? UiTextAlignment::center
                      : UiTextAlignment::start))
            return false;
    }
    return true;
}
} // namespace maicord
