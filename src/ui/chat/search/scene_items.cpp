#include "scene_internal.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/chat/messages/presentation.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/ui/chat/composer/attachment_actions.hpp"
#include "maicord/ui/voice/audio_controls.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord {
using namespace search_scene_detail;

namespace search_scene_detail {
namespace {
constexpr float default_body_height = 48;
} // namespace
bool rich_enabled(const UiSearchScene& s) noexcept {
    return s.source && s.source->rich_timeline;
}
const MessageRichRecord* rich_scoped(const MessageRichStore& store, u64 message_id, bool snapshot) noexcept {
    return snapshot ? message_rich_store_snapshot(store, message_id)
                    : message_rich_store_find(store, message_id);
}
namespace {
void block_input(UiWidgetManager* widgets, UiWidgetHandle handle, bool blocked) noexcept {
    if (auto* widget = ui_widget_manager_resolve(widgets, handle)) {
        if (blocked)
            widget->flags |= ui_widget_block_input;
        else
            widget->flags &= static_cast<u16>(~ui_widget_block_input);
    }
}
UiSearchTarget source_target(const UiSearchScene& s, UiSearchAction action, u64 channel_id, u64 message_id,
                             u32 source_index) noexcept {
    UiSearchTarget target{action, channel_id, message_id};
    target.source_revision = s.source_revision;
    target.source_index = source_index;
    return target;
}
} // namespace
UiSearchTarget rich_target(const UiSearchScene& s, u64 channel_id, u64 message_id, u32 source_index,
                           MessageSceneRichItemKind kind, u32 index, bool snapshot) noexcept {
    auto target = source_target(s, UiSearchAction::rich_item, channel_id, message_id, source_index);
    target.rich_kind = kind;
    target.rich_index = static_cast<u16>(index);
    target.snapshot = snapshot;
    return target;
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
            block_input(&scene.widgets, row->widget, redacted);
            block_input(&scene.widgets, row->poll, redacted);
            if (redacted)
                continue;
            const auto* rich = rich_scoped(*scene.timeline->rich_store, row->message_id, child != 0);
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
        if (!arena_initialize(&s.rich_projection_storage, sizeof(UiSearchRichProjection), 0, 4096, ArenaTag::ui))
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
        static constexpr UiIcon icons[] = {UiIcon::image, UiIcon::file_video, UiIcon::file_audio,
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
namespace {
float choice_em(const UiSearchScene& scene) noexcept {
    return scene.theme_styles && scene.theme_styles->ready ? scene.theme_styles->typography.body_size
                                                           : 16.f;
}
} // namespace
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
bool choice_menu_geometry(const UiSearchScene& scene, const UiRect& rectangle,
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
UiRect choice_avatar_rectangle(const UiMenuRowGeometry& geometry, const UiRect& row) noexcept {
    const float cx = (geometry.icon.left + geometry.icon.right) * .5f;
    const float cy = (row.top + row.bottom) * .5f;
    const float half = minimum(12.f, maximum(ui_rect_width(geometry.icon), ui_rect_height(geometry.icon)) * .5f + 2.f);
    return {cx - half, cy - half, cx + half, cy + half};
}
// Tab presentation and the per-item layouts of the Media, Files and Links tabs.
Utf8View tab_label(u32 tab) noexcept {
    static constexpr Utf8View labels[] = {utf8_literal("Messages"), utf8_literal("Media"), utf8_literal("Files"),
                                   utf8_literal("Links"), utf8_literal("Pins")};
    return tab < 5 ? labels[tab] : Utf8View{};
}
namespace {
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
    u8 used = static_cast<u8>(decimal_write(out, unit ? scaled / 10 : bytes));
    if (unit && scaled < 100 && scaled % 10) {
        out[used++] = '.';
        out[used++] = static_cast<char>('0' + scaled % 10);
    }
    out[used++] = ' ';
    for (const char* u = units[unit]; *u; ++u)
        out[used++] = *u;
    return used;
}
} // namespace
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
static bool content_link(Utf8View text, u32* offset, u32* bytes) noexcept {
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
namespace {
MessageHandle source_handle(const UiSearchScene& s, u32 index) noexcept {
    return s.source->rich_order.count
               ? s.source->rich_order[index]
               : message_store_ordered_handle(*s.source->rich_timeline->store, index);
}
const MessageRichRecord* item_rich(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const auto& store = *s.source->rich_timeline->rich_store;
    return rich_scoped(store, item.message_id, item.snapshot);
}
const MessageRichAttachment* item_attachment(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const auto* rich = item.kind == UiSearchItemKind::attachment ? item_rich(s, item) : nullptr;
    return rich && item.rich_index < rich->attachment_count ? &rich->attachments[item.rich_index] : nullptr;
}
const MessageRichEmbed* item_embed(const UiSearchScene& s, const UiSearchItem& item) noexcept {
    const auto* rich = item.kind == UiSearchItemKind::embed ? item_rich(s, item) : nullptr;
    return rich && item.rich_index < rich->embed_count ? &rich->embeds[item.rich_index] : nullptr;
}
} // namespace
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
MessageResultCardHeaderLayout plain_header(const UiRect& card, const UiSearchRow& row) noexcept {
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
namespace {
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
            reset_value(&item);
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
            const auto* rich = rich_scoped(store, record->id, scope != 0);
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
} // namespace
// Images fill their tile: the quad covers it and the tile clips.
UiRect tile_cover(const UiSearchScene& s, const UiSearchItem& item, const UiRect& tile) noexcept {
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
    auto target = source_target(s, UiSearchAction::result, item.channel_id, item.message_id, item.source_index);
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
const UiSearchItem* target_item(const UiSearchScene& s, const UiSearchTarget& target,
                                u32* slot) noexcept {
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
namespace {
bool row_text_valid(const UiSearchRow& row) noexcept {
    return row.channel_id && row.message_id && utf8_validate(row.body).valid &&
           utf8_validate(row.author).valid && utf8_validate(row.channel).valid &&
           utf8_validate(row.detail).valid;
}
} // namespace
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
        // The range test also rejects NaN and infinities.
        if (!row_text_valid(row) ||
            !(row.body_height == 0 || (row.body_height >= 24 && row.body_height <= 464)))
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
namespace {
UiWidgetHandle add(UiSearchScene& s, const UiRect& rectangle, const UiSearchTarget& target, const Utf8View& name,
                   bool enabled = true, UiWidgetHandle parent = ui_widget_invalid_handle()) noexcept {
    const auto origin = parent.generation ? ui_bounds(s, parent) : widget_viewport(s);
    const auto result = ui_widget_manager_add_placed(
        &s.widgets, parent.generation ? parent : s.root,
        target.action == UiSearchAction::edit ? UiWidgetKind::text_field : UiWidgetKind::button,
        static_cast<u16>(enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                 : ui_widget_none),
        name, rectangle, origin);
    if (!result.ok())
        return ui_widget_invalid_handle();
    s.targets[result.handle.slot] = target;
    return result.handle;
}
UiWidgetHandle add_action(UiSearchScene& s, const UiRect& rectangle, UiSearchAction action, const Utf8View& name,
                          bool enabled = true, u32 source_index = ~u32{0}) noexcept {
    UiSearchTarget target{action, 0, 0};
    target.source_index = source_index;
    return add(s, rectangle, target, name, enabled);
}
} // namespace
void count_label(UiSearchScene& s) noexcept {
    UiLabelWriter writer{s.count_text, &s.count_bytes};
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
        writer << heading;
        return;
    }
    if (s.content->total_estimated)
        writer << utf8_literal("About ");
    writer << static_cast<u64>(s.content->total) << utf8_literal(" results");
}
UiRect overlay_region(const UiSearchScene& s, const UiRect& rectangle) noexcept {
    auto region = s.body;
    if (chooser_open(s) && ui_rect_intersects(rectangle, s.chooser))
        region.top = maximum(region.top, s.chooser.bottom);
    if (s.content->sort_menu && ui_rect_intersects(rectangle, s.sort_chooser)) {
        if (rectangle.left < s.sort_chooser.left)
            region.right = minimum(region.right, s.sort_chooser.left);
        else
            region.top = maximum(region.top, s.sort_chooser.bottom);
    }
    return region;
}
UiRect overlay_clip(const UiSearchScene& s, const UiRect& rectangle) noexcept {
    return ui_rect_intersection(rectangle, overlay_region(s, rectangle));
}
bool menu_hit(const UiSearchScene& s, float x, float y) noexcept {
    return (chooser_open(s) && ui_rect_contains(s.chooser, x, y)) ||
           (s.content->sort_menu && ui_rect_contains(s.sort_chooser, x, y));
}
namespace {
bool menus_closed(const UiSearchScene& s) noexcept {
    return !chooser_open(s) && !s.content->sort_menu;
}
UiRect item_save_rectangle(const UiRect& row) noexcept {
    return ui_rect_edges(row, ui_end(-44), ui_start(12), ui_end(-12), ui_start(44));
}
} // namespace
UiRect item_jump_rectangle(const UiRect& row) noexcept {
    return ui_rect_edges(row, ui_end(-64), ui_start(16), ui_end(-12), ui_start(40));
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
namespace {
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
    static constexpr struct {
        char suffix[6];
        u8 bytes;
        UiIcon icon;
    } kSuffixes[] = {{".zip", 4, UiIcon::file_archive}, {".rar", 4, UiIcon::file_archive},
                     {".7z", 3, UiIcon::file_archive},  {".tar", 4, UiIcon::file_archive},
                     {".gz", 3, UiIcon::file_archive},  {".txt", 4, UiIcon::file_text},
                     {".md", 3, UiIcon::file_text},     {".pdf", 4, UiIcon::file_text},
                     {".log", 4, UiIcon::file_text},    {".json", 5, UiIcon::file_text}};
    for (const auto& entry : kSuffixes)
        if (ends(entry.suffix, entry.bytes))
            return entry.icon;
    return UiIcon::file;
}
bool add_items(UiSearchScene& s) noexcept {
    auto& p = *s.rich_projection;
    p.visible_count = 0;
    const bool enabled = menus_closed(s);
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
        reset_value(&p.secondary[v]);
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
} // namespace
bool paint_items(PaintStream* out, const UiSearchScene& s, const Theme& theme,
                 MessageMediaCommandStream* media) noexcept {
    UiPaint p{out, theme};
    const auto& proj = *s.rich_projection;
    const auto& timeline = *s.source->rich_timeline;
    const auto& store = *timeline.rich_store;
    for (u32 v = 0; p && v < proj.visible_count; ++v) {
        const auto& item = proj.items[proj.visible[v]];
        const auto screen = proj.screens[v];
        const auto clip = overlay_clip(s, screen);
        const bool hover = ui_hovered(s, proj.primary[v]), focus = ui_focused(s, proj.primary[v]);
        if (s.layout == UiSearchLayout::grid) {
            const MessageMediaObscure obscure = item_obscure(s, item);
            // Tiles round with the card radius token; media quads are masked to the same corners.
            const float tile_radius = theme.scale.radius_md;
            p.rounded(screen, theme.surface_high, tile_radius);
            if (message_media_obscure_notice(obscure)) {
                const float cx = (screen.left + screen.right) * .5f, cy = (screen.top + screen.bottom) * .5f;
                p.icon(UiIcon::eye_off, {cx - 12, cy - 12, cx + 12, cy + 12}, theme.text_secondary);
            } else if (p && media && media->storage.data && media->count < media->storage.count &&
                       ui_rect_has_area(clip)) {
                const auto* record = message_store_resolve(*timeline.store, source_handle(s, item.source_index));
                MessageMediaCommand command{};
                command.rectangle = tile_cover(s, item, screen);
                command.clip = clip;
                command.owner = {&s.widgets, proj.primary[v]};
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
            if (hover || focus)
                p.rounded_border({screen.left - 2, screen.top - 2, screen.right + 2, screen.bottom + 2}, Color{},
                                 focus ? theme.tokens.border.focus : theme.interactive, tile_radius + 2, 2);
            continue;
        }
        p.use<ui_background_paint_surface>(screen, theme, hover ? theme.surface_high : theme.surface,
                                           focus ? theme.tokens.border.focus : theme.surface_high,
                                           theme.scale.radius_md, 1.f, 1.f, kGradient, UiBackgroundTier::low,
                                           kSemantic);
        const UiRect icon_box = ui_rect_edges(screen, ui_start(12), ui_start(12), ui_start(44), ui_start(44));
        p.rounded(icon_box, theme.surface_high, 6)
            .icon(item_icon(s, item), ui_rect_shrink(icon_box, 6),
                  s.layout == UiSearchLayout::links ? theme.tokens.emphasis.outline : theme.text_secondary);
        if (!proj.secondary[v].generation || !ui_widget_manager_layout_node(s.widgets, proj.secondary[v]))
            continue;
        const auto r =
            s.layout == UiSearchLayout::files ? item_save_rectangle(screen) : item_jump_rectangle(screen);
        const bool active = ui_hovered(s, proj.secondary[v]) || ui_focused(s, proj.secondary[v]);
        p.rounded(r, active ? ui_background_hover_color(theme, theme.interactive_hover, .28f) : theme.surface_high, 6);
        if (s.layout == UiSearchLayout::files)
            p.icon(UiIcon::download, ui_rect_shrink(r, 6), active ? theme.text_primary : theme.text_secondary);
    }
    return p.end();
}
bool paint_tabs(PaintStream* out, const UiSearchScene& s, const Theme& theme) noexcept {
    if (!s.content->tabs)
        return true;
    UiPaint p{out, theme};
    const auto panel = s.panel;
    p.separator({panel.left + 8, panel.top + 115, panel.right - 8, panel.top + 116});
    for (u32 i = 0; p && i < 5; ++i) {
        const auto r = ui_bounds(s, s.tabs[i]);
        if (!ui_rect_has_area(r))
            continue;
        if (ui_hovered(s, s.tabs[i]) || ui_focused(s, s.tabs[i]))
            p.rounded({r.left, r.top, r.right, r.bottom - 3},
                      ui_background_hover_color(theme, theme.interactive_hover, .18f), 6);
        if (s.content->tab == i)
            p.rounded({r.left + 8, r.bottom - 3, r.right - 8, r.bottom}, theme.tokens.emphasis.outline, 1.5f);
    }
    return p.end();
}
bool rebuild(UiSearchScene& s) noexcept {
    const auto old_focus = s.widgets.focused;
    const UiSearchTarget focus = old_focus.generation && old_focus.slot < s.storage.widgets.count
                                     ? s.targets[old_focus.slot]
                                     : UiSearchTarget{};
    clear_bytes(s.targets, s.storage.widgets.count * sizeof(UiSearchTarget));
    s.root = ui_scene_root(&s.widgets, utf8_literal("Message search"));
    if (!s.root.generation)
        return false;
    s.edit = add_action(s, s.field, UiSearchAction::edit, utf8_literal("Search messages"));
    // After the edit: the chip wins hit tests over the field it sits in.
    s.scope = ui_rect_has_area(s.scope_chip)
                  ? add_action(s, s.scope_chip, UiSearchAction::scope, utf8_literal("Remove search scope"))
                  : UiWidgetHandle{};
    const UiRect submit{s.field.right - 32, s.field.top, s.field.right, s.field.bottom};
    s.buttons[0] = add_action(s, submit, UiSearchAction::submit, utf8_literal("Search"));
    s.count = 0;
    s.first = 0;
    s.page_count = 0;
    if (s.content->panel) {
        const auto p = s.panel;
        // Icon button (× with a "Close search" tooltip), a 32-DIP square like the header buttons.
        s.buttons[1] = add_action(s, ui_search_close_rectangle(p), UiSearchAction::close,
                                  utf8_literal("Close search"));
        s.buttons[2] = add_action(s, ui_rect_edges(p, ui_end(-196), ui_start(40), ui_end(-124), ui_start(72)),
                                  UiSearchAction::filters, label(UiSearchAction::filters));
        s.buttons[8] = add_action(s, ui_rect_edges(p, ui_end(-120), ui_start(40), ui_end(-8), ui_start(72)),
                                  UiSearchAction::sort, label(UiSearchAction::sort));
        for (u32 i = 0; i < 5; ++i) {
            reset_value(&s.tabs[i]);
            if (!s.content->tabs)
                continue;
            const float width = (ui_rect_width(p) - 16) / 5;
            s.tabs[i] = add_action(
                s, {p.left + 8 + i * width, p.top + 76, p.left + 8 + (i + 1) * width, p.top + 116},
                UiSearchAction::tab, tab_label(i), menus_closed(s), i);
        }
        const UiSearchAction sorts[] = {UiSearchAction::newest, UiSearchAction::oldest,
                                        UiSearchAction::relevance};
        for (u32 i = 0; i < 3; ++i)
            reset_value(&s.buttons[3 + i]);
        s.buttons[6] = add_action(s, ui_rect_edges(p, ui_start(8), ui_end(-36), ui_start(76), ui_end(-8)),
                                  UiSearchAction::previous, label(UiSearchAction::previous),
                                  s.content->previous);
        s.buttons[7] = add_action(s, ui_rect_edges(p, ui_end(-72), ui_end(-36), ui_end(-8), ui_end(-8)),
                                  UiSearchAction::next, label(UiSearchAction::next), s.content->next);
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
                if (!s.source->resolve(s.source->context, i, &row) || !row_text_valid(row))
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
                UiSearchTarget target = source_target(s, UiSearchAction::result, row.channel_id, row.message_id, i);
                s.rows[s.count] = add(
                    s, rich ? ui_rect_intersection(jump, s.body) : r, target,
                    rich ? (row.access_check ? utf8_literal("Check access") : utf8_literal("Jump"))
                         : row.author,
                    (!row.unavailable || row.access_check) && menus_closed(s));
                if (rich && !row.unavailable) {
                    const auto& native = s.rich_projection->scene;
                    const auto* outer = message_scene_find_row(native, i);
                    const MessageSceneRowWidget* projected[]{
                        outer, outer ? message_scene_snapshot_row(native, *outer) : nullptr};
                    for (u32 child = 0; child < 2; ++child) {
                        const auto* rich_row = projected[child];
                        const auto* record = !rich_row ? nullptr
                                             : rich_scoped(*native.timeline->rich_store,
                                                           rich_row->message_id, child != 0);
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
                                const UiAudioControlsStyle audio_style = ui_theme_audio_style(native.theme_styles);
                                if (!ui_audio_card_geometry(node->content_rectangle, audio_style,
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
                            auto save_target = rich_target(s, row.channel_id, row.message_id, i,
                                                           MessageSceneRichItemKind::attachment, item,
                                                           child != 0);
                            save_target.save = true;
                            if (!add(s, save, save_target, utf8_literal("Save attachment"),
                                     menus_closed(s))
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
                s.buttons[3 + i] = add_action(s, {s.sort_chooser.left + 4, top, s.sort_chooser.right - 4, bottom},
                                              sorts[i], label(sorts[i]));
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
                add_action(s, {s.chooser.left + 4, top, s.chooser.right - 4, bottom}, action, label(action));
            }
        if (chooser_open(s) && recent_shown(s)) {
            const float heading = recent_top(s);
            if (heading + 28 <= s.chooser.bottom - 4 &&
                !add_action(s, {s.chooser.right - 68, heading, s.chooser.right - 8, heading + 28},
                            UiSearchAction::clear_recent, label(UiSearchAction::clear_recent))
                     .generation)
                return false;
            for (u32 i = 0; i < s.content->recent_count; ++i) {
                const float top = heading + 28 + i * 36, bottom = minimum(top + 36, s.chooser.bottom - 4);
                if (bottom <= top)
                    break;
                if (!add_action(s, {s.chooser.left + 4, top, s.chooser.right - 4, bottom},
                                UiSearchAction::recent, s.content->recent[i], true, i)
                         .generation)
                    return false;
            }
        }
    }
    if (!ui_scene_commit(&s.widgets, widget_viewport(s), s.targets, focus, ui_search_target_equal, &s.revision))
        return false;
    count_label(s);
    return true;
}
UiRect rich_item_clip(const UiSearchScene& s, const UiRect& rectangle, const UiRect& node_clip) noexcept {
    return ui_rect_intersection(ui_search_scene_content_clip(s, rectangle), node_clip);
}
} // namespace search_scene_detail
} // namespace maicord
