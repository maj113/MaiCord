#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/voice/audio_controls.hpp"
#include "maicord/model/chat/message_poll_store.hpp"

#include "scene_internal.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord {
using namespace search_scene_detail;
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
        p.layout_passes == scene->layout_passes && !message_scene_pending(*scene))
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
bool ui_search_scene_target_current(const UiSearchScene& s, const UiSearchTarget& target) noexcept {
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
    const auto* rich = rich_scoped(store, target.message_id, target.snapshot);
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
    if (ui_widget_handle_equal(outer->poll, handle)) {
        const auto target = rich_target(s, record->channel_id, record->id, outer->source_index,
                                        MessageSceneRichItemKind::poll, 0, false);
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
                auto target = rich_target(s, record->channel_id, record->id, outer->source_index,
                                          static_cast<MessageSceneRichItemKind>(kind), index, child != 0);
                if (target.rich_kind == MessageSceneRichItemKind::embed) {
                    const auto& store = *scene->timeline->rich_store;
                    const auto* rich = rich_scoped(store, record->id, child != 0);
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
UiRect ui_search_scene_content_clip(const UiSearchScene& s, const UiRect& rectangle) noexcept {
    // Item layouts hide the message projection, including its text and emoji.
    if (!current(s) || tiled(s))
        return {};
    return overlay_clip(s, rectangle);
}
namespace {
const MessageSceneRowWidget* rich_target_row(const UiSearchScene& s, const UiSearchTarget& target,
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
} // namespace
bool ui_search_scene_rich_preview_geometry(const UiSearchScene& s, const UiSearchTarget& target, UiRect* rectangle,
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
    const auto* rich = rich_scoped(store, target.message_id, target.snapshot);
    if (!rich)
        return false;
    if (target.rich_kind == MessageSceneRichItemKind::attachment &&
        target.rich_index < rich->attachment_count) {
        const auto& attachment = rich->attachments[target.rich_index];
        if (message_rich_attachment_audio_supported(store, attachment)) {
            UiAudioCardGeometry geometry{};
            const UiAudioControlsStyle audio_style = ui_theme_audio_style(scene->theme_styles);
            if (!ui_audio_card_geometry(node->content_rectangle, audio_style,
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
        const float extent = kDefaultMessageStickerLayoutMetrics.extent;
        *rectangle = {node->rectangle.left, node->rectangle.top, node->rectangle.left + extent,
                      node->rectangle.top + extent};
    } else
        return false;
    *clip = rich_item_clip(s, *rectangle, node->clip_rectangle);
    return ui_rect_has_area(*clip);
}
bool ui_search_scene_update(UiSearchScene* s, const UiSearchSource* source, const UiSearchContent* content,
                            const UiRect& viewport, const UiRect& title, const UiShellHeaderGeometry* header) noexcept {
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
    const u32 source_revision = source ? source->revision : 0;
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
    const bool warm = !s->media_policy_dirty && same_choices && s->active && s->source == source &&
                      s->source_revision == source_revision && ui_rect_equal(s->viewport, viewport) &&
                      ui_rect_equal(s->field, field) && ui_rect_equal(s->text, text_area);
    if (warm && s->content == content && s->content_revision == content->revision)
        return true;
    if (warm && s->content_layout == menu_layout && s->pagination_total == content->total &&
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
        if (!ui_scene_open(&s->storage, &s->widgets, widget_capacity, widget_capacity * sizeof(UiSearchTarget),
                           alignof(UiSearchTarget)))
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
                              (recent ? 32 + content->recent_count * 36 : 0))};
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
    s->source_revision = source_revision;
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
    clear_bytes(s, sizeof(*s));
    s->choice_selected = kUiSearchFilterNoSelection;
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
                                          UiRect* clip) noexcept {
    UiRect row{}, row_clip{};
    UiMenuRowGeometry geometry{};
    if (!rectangle || !clip || !ui_search_scene_filter_choice_geometry(scene, source_index, &row, &row_clip) ||
        !choice_person(*scene.choice_source, scene.choice_source->rows[source_index]) ||
        !choice_menu_geometry(scene, row, &geometry))
        return false;
    for (u32 slot = 1; slot < scene.widgets.count; ++slot) {
        const auto& target = scene.targets[slot];
        if (target.action == UiSearchAction::filter_value && target.source_index == source_index &&
            ui_search_scene_target_current(scene, target)) {
            *rectangle = choice_avatar_rectangle(geometry, row);
            *clip = ui_rect_intersection(*rectangle, row_clip);
            return ui_rect_has_area(*clip);
        }
    }
    return false;
}
namespace {
void hit_test(const UiSearchScene& s, float x, float y, UiSearchTarget* out) noexcept {
    if (!current(s))
        return;
    const auto hit = ui_widget_manager_hit_test(s.widgets, x, y);
    const auto target =
        hit.generation && hit.slot < s.storage.widgets.count ? s.targets[hit.slot] : UiSearchTarget{};
    if ((target.action == UiSearchAction::result || target.action == UiSearchAction::rich_item) &&
        menu_hit(s, x, y))
        return;
    if (target.action != UiSearchAction::none) {
        if (ui_search_scene_target_current(s, target))
            *out = target;
        return;
    }
    const auto* rich_scene = tiled(s) ? nullptr : ui_search_scene_message_scene(s);
    if (!rich_scene || !ui_rect_contains(s.body, x, y) ||
        menu_hit(s, x, y))
        return;
    const auto rich_hit = message_scene_hit_test(*rich_scene, x, y);
    const auto* outer =
        rich_hit.message_id ? message_scene_find_row(*rich_scene, rich_hit.source_index) : nullptr;
    if (outer && outer->redacted)
        return;
    const auto* record =
        outer ? message_store_resolve(*rich_scene->timeline->store, outer->message) : nullptr;
    if (!record)
        return;
    if (outer->poll_content_revision) {
        const auto* node = message_scene_layout_node(*rich_scene, outer->poll);
        if (node && ui_rect_contains(rich_item_clip(s, node->rectangle, node->clip_rectangle), x, y)) {
            const auto poll_target = rich_target(s, record->channel_id, record->id, outer->source_index,
                                                 MessageSceneRichItemKind::poll, 0, false);
            if (ui_search_scene_target_current(s, poll_target))
                *out = poll_target;
            return;
        }
    }
    const MessageSceneRowWidget* projected[]{outer, message_scene_snapshot_row(*rich_scene, *outer)};
    for (u32 child = 0; child < 2; ++child) {
        const auto* row = projected[child];
        if (!row)
            continue;
        const auto* rich = rich_scoped(*rich_scene->timeline->rich_store, record->id, child != 0);
        if (!rich)
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
                auto result = rich_target(s, record->channel_id, record->id, outer->source_index, item_kind,
                                          index, child != 0);
                if (item_kind == MessageSceneRichItemKind::attachment &&
                    message_rich_attachment_audio_supported(*rich_scene->timeline->rich_store,
                                                            rich->attachments[index])) {
                    UiAudioCardGeometry geometry{};
                    const UiAudioControlsStyle audio_style = ui_theme_audio_style(rich_scene->theme_styles);
                    if (!ui_audio_card_geometry(node->content_rectangle, audio_style, &geometry,
                                                message_rich_record_voice(*rich)))
                        return;
                    if (ui_rect_contains(geometry.download, x, y))
                        result.save = true;
                    else if (!ui_rect_contains(geometry.play_pause, x, y))
                        return;
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
                            *out = result;
                            return;
                        }
                        if (ui_rect_contains(rich_item_clip(s, block, node->clip_rectangle), x, y))
                            return;
                        *out = result;
                        return;
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
                *out = result;
                return;
            }
    }
}
} // namespace
UiSearchTarget ui_search_scene_hit_test(const UiSearchScene& s, float x, float y) noexcept {
    UiSearchTarget target{};
    hit_test(s, x, y, &target);
    return target;
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
    *clip = overlay_clip(s, *rectangle);
    return ui_rect_has_area(*clip);
}
} // namespace maicord
