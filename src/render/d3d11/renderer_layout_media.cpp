#include "renderer_layout_internal.hpp"

namespace maicord {
namespace renderer_layout_detail {

// Call tiles: the resident video picture (preview atlas), otherwise the
// participant's avatar over its placeholder circle, or a stream's preview.
bool queue_call_media(D3D11Renderer* renderer, const Theme& theme) noexcept {
    const auto& scene = *renderer->call_scene;
    bool first = true;
    for (u32 i = 0; i < scene.tile_count && renderer->media_quad_count < kRendererMediaQuadCapacity &&
                    renderer->media_batch_count < kRendererMediaBatchCapacity;
         ++i) {
        UiCallTileMedia media{};
        if (!ui_call_scene_tile_media(scene, i, &media))
            continue;
        const u32 before = renderer->media_quad_count;
        if (media.video_ssrc && ui_rect_has_area(media.video) && renderer->preview_media_atlas &&
            renderer->media_gpu.preview_texture_view) {
            const auto allocation = media_atlas_peek(renderer->preview_media_atlas,
                                                     media_asset_call_video(media.user_id, media.video_ssrc));
            const auto r = media.video;
            if (allocation.ok() &&
                !queue_media_quad(renderer, allocation, r, media.clip, false, first,
                                  MediaQuadStyle{media.rounded_clip, 1.f, 0.f, media.radius, 0.f, 1}))
                return false;
        }
        if (renderer->media_quad_count == before && media_asset_key_valid(media.preview)) {
            MediaAtlasResult allocation{};
            u8 domain = 0;
            if (find_call_preview(*renderer, media.preview, &allocation, &domain)) {
                const auto r = ui_call_video_fit(media.rounded_clip, allocation.width, allocation.height, false);
                // Under a state scrim the preview is softened as well.
                if (!queue_media_quad(renderer, allocation, r, media.clip, false, first,
                                      MediaQuadStyle{media.rounded_clip, 1.f, 0.f, media.radius, media.dim ? 6.f : 0.f, domain}))
                    return false;
            }
        }
        if (renderer->media_quad_count == before && ui_rect_has_area(media.avatar) && renderer->user_store &&
            renderer->media_atlas && renderer->media_gpu.texture_view) {
            const auto* user = user_store_find(*renderer->user_store, media.user_id);
            MediaAtlasResult allocation{};
            const auto a = media.avatar;
            if (user && find_user_avatar(*renderer, *user, &allocation)) {
                if (!queue_media_quad(renderer, allocation, a, media.clip, true, first))
                    return false;
                reveal_media_underlay(*renderer, before, a, theme.avatar_placeholder);
            }
        }
        first = first && renderer->media_quad_count == before;
    }
    // Stage audience and hand-raise avatars over their placeholders.
    UiCallTileMedia media{};
    for (u32 i = 0; renderer->user_store && renderer->media_atlas && renderer->media_gpu.texture_view &&
                    renderer->media_quad_count < kRendererMediaQuadCapacity &&
                    renderer->media_batch_count < kRendererMediaBatchCapacity &&
                    ui_call_scene_stage_avatar(scene, i, &media);
         ++i) {
        const auto* user = user_store_find(*renderer->user_store, media.user_id);
        MediaAtlasResult allocation{};
        const u32 before = renderer->media_quad_count;
        const auto a = media.avatar;
        if (!user || !find_user_avatar(*renderer, *user, &allocation))
            continue;
        if (!queue_media_quad(renderer, allocation, a, media.clip, true, first))
            return false;
        reveal_media_underlay(*renderer, before, a, theme.avatar_placeholder);
        first = first && renderer->media_quad_count == before;
    }
    return true;
}

// The toast's own batches close every paint stream; d3d11_renderer_draw
// draws them after all other layers.
bool queue_toast(D3D11Renderer* renderer, const Theme& theme) noexcept {
    renderer->toast_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    return !d3d11_renderer_toast_visible(*renderer) ||
           ui_toast_paint(*renderer->toast_scene, theme, &renderer->paint_stream);
}
bool queue_inline_flags(D3D11Renderer* renderer) noexcept {
    renderer->inline_media_first_batch = renderer->media_batch_count;
    for (u32 layer = 0; layer < static_cast<u32>(RendererInlineMediaLayer::count); ++layer) {
        renderer->inline_media_layer_first[layer] = renderer->media_batch_count;
        bool first = true;
        for (u32 i = 0; renderer->inline_media.flags &&
                        i < minimum(renderer->inline_media.count, kRendererInlineFlagCapacity);
             ++i) {
            const auto& flag = renderer->inline_media.flags[i];
            if (static_cast<u32>(flag.layer) != layer || !renderer_inline_flag_current(*renderer, flag) ||
                !renderer->media_atlas || !renderer->media_gpu.texture_view)
                continue;
            const auto allocation =
                media_atlas_peek(renderer->media_atlas, media_asset_regional_flag(flag.flag));
            if (!allocation.ok())
                continue;
            if (renderer->media_quad_count == kRendererMediaQuadCapacity ||
                renderer->media_batch_count == kRendererMediaBatchCapacity)
                break;
            const auto r = flag.rectangle;
            // Message rows clip to themselves; the viewport bounds their flags.
            const UiRect clip = renderer->message_scene && flag.owner.manager == &renderer->message_scene->widgets
                                    ? ui_rect_intersection(flag.clip, renderer->message_scene->viewport)
                                    : flag.clip;
            if (!queue_media_quad(renderer, allocation, r, clip, false,
                                  first))
                return false;
            first = false;
        }
        // A reaction tooltip's custom emoji or flag: over its surface, under
        // its text, fitted to its square.
        UiRect square{};
        if (layer == static_cast<u32>(RendererInlineMediaLayer::form) && d3d11_renderer_tooltip_visible(*renderer) &&
            media_asset_key_valid(renderer->tooltip_scene->emoji_media) &&
            ui_tooltip_emoji(*renderer->tooltip_scene, &square) && renderer->media_atlas &&
            renderer->media_gpu.texture_view && renderer->media_quad_count < kRendererMediaQuadCapacity &&
            renderer->media_batch_count < kRendererMediaBatchCapacity) {
            const auto allocation = media_atlas_peek(renderer->media_atlas, renderer->tooltip_scene->emoji_media);
            if (allocation.ok() && allocation.width && allocation.height) {
                if (!queue_media_quad(renderer, allocation, fit_in(square, allocation.width, allocation.height, false),
                                      renderer->tooltip_scene->viewport, false, first))
                    return false;
                first = false;
            }
        }
        if (layer == static_cast<u32>(RendererInlineMediaLayer::forum)) {
            renderer->forum_media_first_batch = renderer->media_batch_count;
            if (renderer->forum_form && renderer->forum_form->scene &&
                renderer->forum_form->scene->active && renderer->preview_media_atlas &&
                renderer->media_gpu.preview_texture_view) {
                const auto& scene = *renderer->forum_form->scene;
                const auto& list = scene.attachment_list;
                for (u32 row = 0; row < list.rows.count; ++row) {
                    const auto& view = list.rows[row];
                    if (!view.preview_ready || view.spoiler)
                        continue;
                    UiRect clip{};
                    const auto tile = ui_attachment_draft_list_preview(list, row, &clip);
                    if (!ui_rect_has_area(clip))
                        continue;
                    const auto allocation = media_atlas_peek(renderer->preview_media_atlas, view.preview);
                    if (!allocation.ok() || !allocation.width || !allocation.height)
                        continue;
                    if (!queue_media_quad(renderer, allocation, fit_in(tile, allocation.width, allocation.height, false), clip,
                                          false, first, MediaQuadStyle{tile, 1.f, 0.f, list.style.radius, 0.f, 1}))
                        return false;
                    first = false;
                }
            }
            renderer->forum_media_end_batch = renderer->media_batch_count;
        }
        renderer->inline_media_layer_end[layer] = renderer->media_batch_count;
    }
    renderer->inline_media_last_batch = renderer->media_batch_count;
    return true;
}
bool split_paint_quad_boundary(PaintStream* paint, u32 quad, u32* batch_index) noexcept {
    if (quad > paint->count)
        return false;
    for (u32 i = 0; i < paint->batch_count; ++i) {
        auto& batch = paint->batches[i];
        if (quad == batch.first_quad) {
            *batch_index = i;
            return true;
        }
        if (quad > batch.first_quad && quad < batch.first_quad + batch.quad_count) {
            if (paint->batch_count == kMaxPaintBatches)
                return false;
            const auto tail = PaintBatch{quad, batch.first_quad + batch.quad_count - quad, batch.clip_index,
                                         batch.reserved};
            batch.quad_count = quad - batch.first_quad;
            for (u32 j = paint->batch_count; j > i + 1; --j)
                paint->batches[j] = paint->batches[j - 1];
            paint->batches[i + 1] = tail;
            ++paint->batch_count;
            *batch_index = i + 1;
            return true;
        }
    }
    // A boundary at the end of the stream closes the last batch: later
    // quads belong to the next range.
    *batch_index = paint_stream_seal(paint);
    return true;
}
u64 shell_segment_key(const RendererPresentationStamp& stamp, const MessageComposerPresentationInput& composer,
                      const PaintStream& paint) noexcept {
    // The composer's caret and selection reach the chrome as geometry; the
    // stamp sees them only through content text, which is a timeline input.
    u64 key = hash64_mix(stamp.shell, stamp.shell_count);
    key = hash64_counted_bytes(key, composer.selections.data,
                               composer.selections.count * static_cast<u32>(sizeof(UiRect)));
    key = hash64_counted_bytes(key, &composer.caret, sizeof(composer.caret));
    key = hash64_mix(key, (composer.geometry_active ? 1u : 0u) | (composer.caret_visible ? 2u : 0u));
    key = hash64_counted_bytes(key, composer.composition.data,
                               composer.composition.count * static_cast<u32>(sizeof(TextCompositionMark)));
    for (u32 i = 0; i < composer.chips.count; ++i) {
        key = hash64_counted_bytes(key, &composer.chips[i].key, 2 * static_cast<u32>(sizeof(UiRect)));
        key = hash64_mix(key, composer.chips[i].flags);
    }
    key = hash64_counted_bytes(key, &paint.scrollbar_viewport, sizeof(paint.scrollbar_viewport));
    return key | 1;
}

// Records the shell segment just built: paint [0, count) with its clips and
// batches, and media [0, count). The chrome regions are those before the
// thread header narrowed the message view.
void shell_segment_capture(D3D11Renderer* renderer, u64 key, const ShellPresentationRegions& regions) noexcept {
    RendererShellSegment& segment = renderer->shell_segment;
    segment.valid = false;
    if (!key || renderer->paint_stream.rejected || renderer->paint_stream.clip_depth != 1)
        return;
    if (!renderer_frame_copy_record(&segment.frame, *renderer)) {
        if (!segment.frame.paint_quads.arena.base)
            d3d11_renderer_shell_segment_release(&segment);
        return;
    }
    segment.message_view = regions.message_view;
    segment.member_sidebar = regions.member_sidebar;
    segment.member_sidebar_visible = regions.member_sidebar_visible;
    segment.key = key;
    segment.valid = true;
    saturating_increment(segment.builds);
}

// Restores the recorded shell segment into the reset streams. Its media keep
// the per-frame work of queue_media_quad: atlas pins (eviction safety) and
// current sources (animation frames, atlas growth).
bool shell_segment_replay(D3D11Renderer* renderer, u64 key, float width_dip, float height_dip,
                          ShellPresentationRegions* regions) noexcept {
    RendererShellSegment& segment = renderer->shell_segment;
    const RendererFrameCopy& frame = segment.frame;
    if (!segment.valid || segment.key != key)
        return false;
    PaintStream& paint = renderer->paint_stream;
    const auto abandon = [&]() noexcept {
        segment.valid = false;
        renderer->media_quad_count = 0;
        renderer->media_batch_count = 0;
        return false;
    };
    if (!paint_stream_reset(&paint, width_dip, height_dip) ||
        (frame.paint_count > paint.quad_capacity &&
         !stable_storage_grow(&paint.quad_storage, frame.paint_count)) ||
        !renderer_media_draw_storage_reserve_quads(&renderer->media_draw, maximum(frame.media_count, 1u)) ||
        !renderer_media_draw_storage_reserve_batches(&renderer->media_draw, maximum(frame.media_batch_count, 1u)))
        return abandon();
    paint.quad_capacity = paint.quad_storage.capacity;
    CopyMemory(paint.quads, stable_storage_data<PaintQuad>(frame.paint_quads),
               static_cast<usize>(frame.paint_count) * sizeof(PaintQuad));
    CopyMemory(paint.clips, frame.clips, static_cast<usize>(frame.clip_count) * sizeof(PaintClip));
    CopyMemory(paint.batches, frame.batches, static_cast<usize>(frame.batch_count) * sizeof(PaintBatch));
    paint.count = frame.paint_count;
    paint.clip_count = frame.clip_count;
    paint.batch_count = frame.batch_count;
    CopyMemory(renderer->media_draw.quad_data, stable_storage_data<MediaQuad>(frame.media_quads),
               static_cast<usize>(frame.media_count) * sizeof(MediaQuad));
    CopyMemory(renderer->media_draw.handle_data, stable_storage_data<MediaAtlasHandle>(frame.media_handles),
               static_cast<usize>(frame.media_count) * sizeof(MediaAtlasHandle));
    CopyMemory(renderer->media_draw.domain_data, stable_storage_data<u8>(frame.media_domains),
               frame.media_count);
    CopyMemory(renderer->media_draw.batch_data, stable_storage_data<MediaDrawBatch>(frame.media_batches),
               static_cast<usize>(frame.media_batch_count) * sizeof(MediaDrawBatch));
    renderer->media_quad_count = frame.media_count;
    renderer->media_batch_count = frame.media_batch_count;
    for (u32 i = 0; i < frame.media_count; ++i) {
        const u8 domain = renderer->media_draw.domain_data[i];
        MediaAtlas* atlas = domain == 0 ? renderer->media_atlas : renderer->preview_media_atlas;
        MediaAtlasResult visible{};
        bool changed = false;
        if (domain > 1 || !media_atlas_resolve_visible(atlas, renderer->media_draw.handle_data[i], &visible) ||
            !update_media_quad_source(renderer, visible, domain, &renderer->media_draw.quad_data[i], &changed))
            return abandon();
    }
    *regions = ShellPresentationRegions{segment.message_view, segment.member_sidebar,
                                        segment.member_sidebar_visible};
    saturating_increment(segment.replays);
    return true;
}

bool queue_message_media(D3D11Renderer* renderer, const Theme& theme,
                         const MessageMediaCommandStream& stream, bool first) noexcept {
    for (u32 index = 0; index < stream.count; ++index) {
        const MessageMediaCommand& command = stream.storage[index];
        MediaAssetKey key{};
        const bool large_preview = command.kind == MessageMediaCommandKind::attachment ||
                                   command.kind == MessageMediaCommandKind::embed;
        const MediaResolutionTier resolution =
            large_preview && renderer->preview_media_atlas
                ? media_resolution_tier_for_display(
                      command.rectangle.right - command.rectangle.left,
                      command.rectangle.bottom - command.rectangle.top, renderer->dpi,
                      media_preview_extent_cap(static_cast<MediaAttachmentFormat>(command.format),
                                               command.animated))
                : MediaResolutionTier::original;
        switch (command.kind) {
        case MessageMediaCommandKind::attachment:
            key = command.search_result
                      ? media_asset_search_attachment(
                            command.id, static_cast<MediaAttachmentFormat>(command.format), resolution,
                            command.message_revision, command.snapshot)
                      : media_asset_attachment(
                            command.id, static_cast<MediaAttachmentFormat>(command.format), resolution);
            break;
        case MessageMediaCommandKind::embed:
            key = command.search_result
                      ? media_asset_search_embed(
                            command.id, command.message_revision, command.embed_index,
                            static_cast<MediaEmbedResource>(command.embed_resource),
                            static_cast<MediaAttachmentFormat>(command.format), command.animated,
                            resolution, command.snapshot)
                      : media_asset_embed(command.id, command.message_revision, command.embed_index,
                                          static_cast<MediaEmbedResource>(command.embed_resource),
                                          static_cast<MediaAttachmentFormat>(command.format),
                                          command.animated, resolution, command.snapshot);
            break;
        case MessageMediaCommandKind::sticker:
            key = media_asset_sticker(command.id, command.format);
            break;
        case MessageMediaCommandKind::custom_emoji:
            key = media_asset_custom_emoji(command.id, command.animated);
            break;
        case MessageMediaCommandKind::regional_flag:
            key = media_asset_regional_flag(static_cast<u16>(command.id));
            break;
        case MessageMediaCommandKind::component:
            key = message_component_media_key(command.id, static_cast<MediaAttachmentFormat>(command.format));
            break;
        }
        const u8 domain = large_preview && renderer->preview_media_atlas ? 1 : 0;
        MediaAtlas* atlas = domain == 1 ? renderer->preview_media_atlas : renderer->media_atlas;
        ID3D11ShaderResourceView* texture_view =
            domain == 1 ? renderer->media_gpu.preview_texture_view : renderer->media_gpu.texture_view;
        if (!atlas || !texture_view) {
            continue;
        }
        const auto playing = renderer->inline_embed_video;
        const bool playing_embed = command.kind == MessageMediaCommandKind::embed && playing.id == key.id &&
            playing.variant == key.variant && playing.kind == MediaAssetKind::embed &&
            media_asset_embed_index(playing) == command.embed_index &&
            media_asset_embed_snapshot(playing) == command.snapshot &&
            media_asset_search(playing) == command.search_result &&
            media_atlas_peek(atlas, playing).ok();
        if (playing_embed)
            key = playing;
        const MediaAtlasResult allocation = media_atlas_peek(atlas, key);
        if (!allocation.ok()) {
            continue;
        }
        if (renderer->media_quad_count >= kRendererMediaQuadCapacity) {
            continue;
        }
        const u32 media_before = renderer->media_quad_count;
        // Grid tiles: the cover rectangle's centred square is the tile itself.
        UiRect tile{};
        if (command.tile_radius) {
            const float side = minimum(ui_rect_width(command.rectangle), ui_rect_height(command.rectangle));
            const float cx = (command.rectangle.left + command.rectangle.right) * .5f,
                        cy = (command.rectangle.top + command.rectangle.bottom) * .5f;
            tile = {cx - side * .5f, cy - side * .5f, cx + side * .5f, cy + side * .5f};
        }
        // Spoiler and explicit previews blur in the media shader until revealed.
        const float blur = command.obscure != static_cast<u8>(MessageMediaObscure::none)
                               ? minimum(kMessageMediaObscureBlur,
                                         minimum(ui_rect_width(command.rectangle),
                                                 ui_rect_height(command.rectangle)) * .25f)
                               : 0.f;
        // Gallery tiles: the image covers its tile, cropped by the tile clip.
        const UiRect quad = message_media_picture_rectangle(command, allocation.width, allocation.height, playing_embed);
        const UiRect quad_clip = command.cover || playing_embed
            ? ui_rect_intersection(command.clip, command.rectangle) : command.clip;
        // Framing: a message attachment rounds 8 DIP, embed
        // media 4 DIP; grid and gallery tiles keep their own mask.
        const float radius = command.ordinary_video ? UiVideoSceneStyle{}.preview_radius
                             : command.tile_radius || command.cover ? 0.f
                             : command.kind == MessageMediaCommandKind::attachment ? theme.scale.radius_md
                             : command.kind == MessageMediaCommandKind::embed ? theme.scale.radius_sm
                                                                              : 0.f;
        if (!queue_media_quad(renderer, allocation, quad, quad_clip, false, first,
                              MediaQuadStyle{tile, 1.f, radius, static_cast<float>(command.tile_radius), blur,
                                             domain})) {
            return layout_step_failed(__LINE__);
        }
        if (large_preview || command.kind == MessageMediaCommandKind::sticker)
            reveal_media_underlay(*renderer, media_before, command.rectangle, theme.surface_high);
        if (renderer->media_quad_count != media_before)
            first = false;
    }
    return true;
}

// Command storage belongs to this surface; producers initialize every consumed record.
bool build_timeline_media(D3D11Renderer* renderer, const Theme& theme, const MessageScene* message_scene,
                          bool messages_visible, bool content_hidden) noexcept {
    LayoutScratch* const scratch = layout_scratch(renderer);
    if (!scratch)
        return layout_step_failed(__LINE__);
    MessageMediaCommandStream message_media_stream{};
    MessagePresentationResult message_presentation{};
    if (messages_visible &&
        (!message_media_command_stream_initialize(
             &message_media_stream,
             Slice<MessageMediaCommand>{scratch->media_commands, kRendererMessageMediaCommandCapacity}) ||
         !message_presentation_build(*renderer->message_scene, theme, &renderer->paint_stream,
                                     &message_media_stream, &message_presentation,
                                     renderer->message_text && renderer->message_text->active ? renderer->message_text
                                                                                              : nullptr))) {
        return layout_step_failed(__LINE__);
    }
    if (messages_visible && renderer->user_store && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        if (renderer->message_text && renderer->chat_state) {
            // A text command's clip, and its widget's, is only its row: header chips and tag icons clip to
            // the scene viewport, as the text pass does, so a row under the channel header paints nothing above it.
            if (!paint_stream_push_clip(&renderer->paint_stream, message_scene->viewport))
                return layout_step_failed(__LINE__);
            for (u32 i = 0; i < renderer->message_text->command_count; ++i) {
                const auto& command = renderer->message_text->commands[i];
                MessageInteractionLayout interaction{};
                if (ui_rect_intersects(command.rectangle, message_scene->viewport) &&
                    message_interaction_layout(*message_scene, command, *renderer->user_store,
                                               renderer->message_measure_cache, theme, &interaction)) {
                    if (!message_interaction_paint(interaction, command, theme, &renderer->paint_stream))
                        return layout_step_failed(__LINE__);
                    if (!interaction.tag.label.bytes)
                        continue;
                    if (!ui_badge_paint(interaction.tag, message_scene->widgets, theme, &renderer->paint_stream))
                        return layout_step_failed(__LINE__);
                    if (interaction.appearance &&
                        !queue_header_tag_icon(renderer, *message_scene, command, *interaction.appearance,
                                               interaction.tag))
                        return layout_step_failed(__LINE__);
                    continue;
                }
                MessageAuthorLayout header{};
                if (!ui_rect_intersects(command.rectangle, message_scene->viewport) ||
                    !message_author_layout(*message_scene, command, *renderer->chat_state,
                                           *renderer->user_store, renderer->message_measure_cache, theme,
                                           &header) ||
                    (!header.badge.label.bytes && !header.plugin.badge.label.bytes && !header.decoration_count))
                    continue;
                if (!ui_plugin_slot_badge_paint(header.plugin, message_scene->widgets, theme, &renderer->paint_stream))
                    return layout_step_failed(__LINE__);
                for (u32 d = 0; d < header.decoration_count; ++d)
                    if (!ui_plugin_slot_badge_paint(header.decorations[d], message_scene->widgets, theme,
                                                    &renderer->paint_stream))
                        return layout_step_failed(__LINE__);
                if (!header.badge.label.bytes)
                    continue;
                if (!ui_badge_paint(header.badge, message_scene->widgets, theme, &renderer->paint_stream))
                    return layout_step_failed(__LINE__);
                if (header.badge.leading && header.appearance &&
                    !queue_header_tag_icon(renderer, *message_scene, command, *header.appearance, header.badge))
                    return layout_step_failed(__LINE__);
            }
            if (!paint_stream_pop_clip(&renderer->paint_stream))
                return layout_step_failed(__LINE__);
        }
        for (u32 index = 0; index < message_scene->count; ++index) {
            const MessageSceneRowWidget& row = message_scene->rows[index];
            if (!message_scene_avatar_visible(*message_scene, row) ||
                renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                continue;
            }
            const MessageRecord* message =
                message_store_resolve(*message_scene->timeline->store, row.message);
            const UserRecord* user =
                message ? user_store_find(*renderer->user_store, message->author_id) : nullptr;
            const UiLayoutNode* avatar = message_scene_layout_node(*message_scene, row.avatar);
            MediaAtlasResult allocation{};
            if (!user || !avatar || !find_user_avatar(*renderer, *user, &allocation)) {
                continue;
            }
            if (!queue_avatar(renderer, allocation, avatar->rectangle, message_scene->viewport,
                              theme.avatar_placeholder))
                return layout_step_failed(__LINE__);
        }
        // A DM's channel-beginning header shows the recipient's avatar.
        if (MessageSceneIntroGeometry intro{};
            message_scene->intro.kind == MessageSceneIntroKind::direct_message && message_scene->intro.user_id &&
            renderer->user_store && renderer->media_quad_count < kRendererMediaQuadCapacity &&
            message_scene_intro_geometry(*message_scene, &intro) && ui_rect_has_area(intro.icon)) {
            const UserRecord* user = user_store_find(*renderer->user_store, message_scene->intro.user_id);
            MediaAtlasResult allocation{};
            const UiRect clip = ui_rect_intersection(message_scene->viewport, intro.area);
            if (user && ui_rect_has_area(clip) && find_user_avatar(*renderer, *user, &allocation) &&
                !queue_avatar(renderer, allocation, intro.icon, clip, theme.avatar_placeholder))
                return layout_step_failed(__LINE__);
        }
        for (u32 index = 0; index < message_scene->count; ++index) {
            if (renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                break;
            }
            const MessageSceneRowWidget& row = message_scene->rows[index];
            const MessageRecord* message =
                message_store_resolve(*message_scene->timeline->store, row.message);
            const MessageRichStore* rich_store = message_scene->timeline->rich_store;
            const MessageRichRecord* rich =
                message && rich_store ? message_rich_store_find(*rich_store, message->id) : nullptr;
            const UserRecord* user =
                !message_scene->display.compact && rich && rich->message_revision == message->revision &&
                        message_rich_has_header(*rich) &&
                        (rich->reference.state != GatewayMessageReferenceState::available ||
                         message_rich_reply_leading(rich->reference) == MessageReplyLeading::avatar)
                    ? user_store_find(*renderer->user_store, message_rich_header_user(*rich))
                    : nullptr;
            const UiLayoutNode* body = message_scene_layout_node(*message_scene, row.body);
            const UiLayoutNode* reply = message_scene_layout_node(*message_scene, row.reply);
            MediaAtlasResult allocation{};
            if (!user || !body || !reply || !find_user_avatar(*renderer, *user, &allocation)) {
                continue;
            }
            const UiRect reference_avatar{body->content_rectangle.left, reply->rectangle.top + 1.0f,
                                          body->content_rectangle.left + 16.0f,
                                          reply->rectangle.top + 17.0f};
            if (!queue_avatar(renderer, allocation, reference_avatar, message_scene->viewport,
                              theme.avatar_placeholder))
                return layout_step_failed(__LINE__);
        }
    }
    if (!content_hidden && renderer->message_composer_scene && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const auto& composer = *renderer->message_composer_scene;
        const auto avatar = message_composer_scene_command_avatar(composer);
        if (ui_rect_has_area(avatar)) {
            const auto allocation = media_atlas_peek(renderer->media_atlas, composer.command.avatar);
            if (allocation.ok() &&
                !queue_media_quad(renderer, allocation, avatar,
                                  composer.viewport, true))
                return layout_step_failed(__LINE__);
        }
        for (u32 i = 0; i < composer.projection_span_count; ++i) {
            UiRect rectangle{}, clip{};
            if (!text_composer_emoji_rectangle(renderer->composer_layout, composer, i, &rectangle, &clip))
                continue;
            const auto& span = composer.projection_spans[i];
            const auto allocation = media_atlas_peek(
                renderer->media_atlas, span.kind == MessageComposerProjectionKind::regional_flag
                                           ? media_asset_regional_flag(static_cast<u16>(span.target))
                                           : media_asset_custom_emoji(span.target, span.animated));
            if (allocation.ok() && !queue_media_quad(renderer, allocation, rectangle, clip))
                return layout_step_failed(__LINE__);
        }
    }
    if (!content_hidden && renderer->content_text && renderer->media_atlas &&
        renderer->media_gpu.texture_view) {
        const RendererTextStorage& text = *renderer->content_text;
        for (u32 index = 0; index < text.content_hit_region_count; ++index) {
            if (renderer->media_quad_count >= kRendererMediaQuadCapacity) {
                break;
            }
            const TextHitRegion& region = text.content_hit_regions[index];
            if (region.semantic_index >= text.content_semantic_count) {
                continue;
            }
            const MessageTextSemantic& semantic = text.content_semantics[region.semantic_index];
            const bool flag = semantic.kind == MessageMarkdownSemanticKind::regional_flag;
            if ((semantic.kind != MessageMarkdownSemanticKind::custom_emoji && !flag) ||
                semantic.target == 0 || (semantic.reserved & (2u | kRendererSearchSemanticFlag))) {
                continue;
            }
            const MediaAtlasResult allocation = media_atlas_peek(renderer->media_atlas, emoji_key(semantic));
            if (!allocation.ok()) {
                continue;
            }
            if (!queue_media_quad(renderer, allocation, emoji_cell(region, flag), message_scene->viewport, false)) {
                return layout_step_failed(__LINE__);
            }
        }
    }

    if (!queue_message_media(renderer, theme, message_media_stream, false))
        return layout_step_failed(__LINE__);
    // Composer upload thumbnails join the ordinary preview layer so the strip
    // overlay (tags, progress and the action bar) paints above them.
    if (!content_hidden && d3d11_renderer_active(renderer->message_composer_scene) &&
        renderer->preview_media_atlas && renderer->media_gpu.preview_texture_view) {
        const auto strip = message_composer_scene_attachment_view(*renderer->message_composer_scene, false);
        bool first_thumbnail = true;
        for (u32 i = 0; i < strip.tiles.count; ++i) {
            UiRect media{}, clip{};
            if (!ui_composer_attachment_thumbnail_visible(strip, i, &media, &clip) ||
                renderer->media_quad_count >= kRendererMediaQuadCapacity)
                continue;
            const auto allocation = media_atlas_peek(renderer->preview_media_atlas, strip.tiles[i].preview);
            if (!allocation.ok() || !allocation.width || !allocation.height)
                continue;
            const UiRect fitted = ui_composer_attachment_fit(media, allocation.width, allocation.height);
            // Spoilered uploads keep their thumbnail, blurred in the media shader.
            const float blur = ui_composer_attachment_thumbnail_blur(strip.tiles[i], fitted);
            if (!queue_media_quad(renderer, allocation, fitted, clip, false, first_thumbnail,
                                  MediaQuadStyle{fitted, 1.f, 0.f, theme.scale.radius_sm, blur, 1}))
                return layout_step_failed(__LINE__);
            first_thumbnail = false;
        }
    }
    if (renderer->content_text) {
        renderer->content_text->message_geometry_quads = message_presentation.geometry_quads;
    }

    // The DM profile panel: its card, then its media with the shell's; its
    // foreground follows above that media, its text with the shell's.
    const ProfileScene* dm_panel =
        !content_hidden && d3d11_renderer_active(renderer->dm_profile_panel) ? renderer->dm_profile_panel : nullptr;
    u32 dm_panel_effects = 0;
    if (dm_panel && (!profile_scene_paint(*dm_panel, theme, &renderer->paint_stream) ||
                     !queue_profile_media(renderer, *dm_panel, theme, &dm_panel_effects)))
        return layout_step_failed(__LINE__);
    // Avatar textures are submitted as media after the base shell geometry.
    // Start a separate paint layer for badges that must remain above those
    // textures regardless of media residency.
    renderer->media_overlay_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (dm_panel && !profile_scene_foreground(*dm_panel, theme, &renderer->paint_stream, nullptr, renderer->dpi))
        return layout_step_failed(__LINE__);
    if (!content_hidden) {
        const UiVideoPlayAffordanceStyle affordance{};
        for (u32 i = 0; i < message_media_stream.count; ++i) {
            const auto& command = message_media_stream.storage[i];
            if (command.obscure != static_cast<u8>(MessageMediaObscure::none)) {
                // A centred scrim pill above the blurred preview; the text
                // pass draws its label. Obscured media offers no save action.
                const UiRect pill = message_media_obscure_pill(command.rectangle,
                                                                static_cast<MessageMediaObscure>(command.obscure));
                if (ui_rect_has_area(pill) &&
                    !paint_stream_push_rounded_border(&renderer->paint_stream, pill.left, pill.top, pill.right,
                                                      pill.bottom, theme.tokens.overlay.scrim_strong,
                                                      theme.tokens.overlay.scrim_strong,
                                                      ui_rect_height(pill) * .5f, 0))
                    return layout_step_failed(__LINE__);
                continue;
            }
            if (command.kind == MessageMediaCommandKind::attachment &&
                !d3d11_renderer_message_modal_active(*renderer) &&
                !ui_attachment_save_paint(
                    &renderer->paint_stream, command.owner,
                    ui_attachment_save_rectangle(command.rectangle, false), command.clip, theme, false,
                    d3d11_renderer_video_scene_visible(*renderer) &&
                        ui_video_scene_covers_preview(*renderer->video_scene, command.rectangle)))
                return layout_step_failed(__LINE__);
            const auto star_owner = renderer->gif_favorite_owner;
            if (command.gif && !d3d11_renderer_message_modal_active(*renderer) && star_owner.generation &&
                command.owner.handle.slot == star_owner.slot &&
                command.owner.handle.generation == star_owner.generation &&
                !ui_gif_favorite_paint(
                    &renderer->paint_stream, command.owner,
                    ui_gif_favorite_rectangle(command.rectangle,
                                              command.kind == MessageMediaCommandKind::attachment),
                    command.clip, theme, renderer->gif_favorite, renderer->gif_favorite_pending))
                return layout_step_failed(__LINE__);
            if (!command.ordinary_video ||
                (d3d11_renderer_video_scene_visible(*renderer) &&
                 ui_video_scene_covers_preview(*renderer->video_scene, command.rectangle)))
                continue;
            if (command.open_link
                    ? !ui_video_embed_actions_paint(command.rectangle, command.clip, true, UiVideoEmbedActionsStyle{},
                                                    theme, &renderer->paint_stream)
                    : !ui_video_play_affordance_paint(command.rectangle, command.clip, affordance, theme,
                                                      &renderer->paint_stream))
                return layout_step_failed(__LINE__);
        }
    }
    if (!content_hidden && d3d11_renderer_active(renderer->message_composer_scene) &&
        !ui_composer_attachment_strip_paint_overlay(
            message_composer_scene_attachment_view(*renderer->message_composer_scene, true), theme,
            &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    if (messages_visible && d3d11_renderer_video_scene_visible(*renderer) &&
        !renderer->video_scene_search_result &&
        !ui_video_scene_paint(*renderer->video_scene, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    // Link chip server icons are the timeline's last media range: they draw
    // after the message text, above a hovered chip's accent pill.
    renderer->chip_media_first_batch = renderer->media_batch_count;
    if (messages_visible && !content_hidden &&
        (!paint_chip_initial_plates(renderer, theme, false) || !queue_chip_guild_icons(renderer, false)))
        return layout_step_failed(__LINE__);
    renderer->chip_media_end_batch = renderer->media_batch_count;
    return true;
}

bool build_search_media(D3D11Renderer* renderer, const Theme& theme) noexcept {
    LayoutScratch* const scratch = layout_scratch(renderer);
    if (!scratch)
        return layout_step_failed(__LINE__);
    MessageMediaCommandStream search_media_stream{};
    if (!message_media_command_stream_initialize(
            &search_media_stream, {scratch->media_commands, kRendererMessageMediaCommandCapacity}))
        return layout_step_failed(__LINE__);
    if (d3d11_renderer_search_visible(*renderer)) {
        const UnfilteredPaint scope(&renderer->paint_stream);
        if (!ui_search_scene_paint(*renderer->search_scene, theme, &renderer->paint_stream,
                                   &renderer->search_edit_geometry, renderer->dpi, &search_media_stream,
                                   renderer->search_message_text && renderer->search_message_text->active
                                       ? renderer->search_message_text : nullptr))
            return layout_step_failed(__LINE__);
    }
    renderer->friends_requests_media_first_batch = renderer->media_batch_count;
    if (!queue_message_media(renderer, theme, search_media_stream, true))
        return layout_step_failed(__LINE__);
    const u32 search_foreground_quad = renderer->paint_stream.count;
    if (d3d11_renderer_search_visible(*renderer))
        renderer->friends_requests_foreground_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (d3d11_renderer_search_visible(*renderer)) {
        const UiVideoPlayAffordanceStyle affordance{};
        for (u32 i = 0; i < search_media_stream.count; ++i) {
            const auto& command = search_media_stream.storage[i];
            const bool covered = d3d11_renderer_video_scene_visible(*renderer) &&
                                 renderer->video_scene_search_result &&
                                 ui_video_scene_covers_preview(*renderer->video_scene, command.rectangle);
            // Media grid tiles are owned by the search scene itself and have no Save action.
            if (command.kind == MessageMediaCommandKind::attachment &&
                command.owner.manager != &renderer->search_scene->widgets &&
                !d3d11_renderer_message_modal_active(*renderer) &&
                !ui_attachment_save_paint(&renderer->paint_stream, command.owner,
                                          ui_attachment_save_rectangle(command.rectangle, false),
                                          command.clip, theme, false, covered))
                return layout_step_failed(__LINE__);
            if (command.ordinary_video && !covered &&
                !ui_video_play_affordance_paint(command.rectangle, command.clip, affordance, theme,
                                                &renderer->paint_stream))
                return layout_step_failed(__LINE__);
        }
        if (d3d11_renderer_video_scene_visible(*renderer) && renderer->video_scene_search_result &&
            !ui_video_scene_paint(*renderer->video_scene, theme, &renderer->paint_stream))
            return layout_step_failed(__LINE__);
        // Rich inline emoji cells belong above Search card geometry.
        if (renderer->content_text && renderer->media_atlas && renderer->media_gpu.texture_view) {
            const auto& text = *renderer->content_text;
            bool first = true;
            for (u32 i = 0; i < text.content_hit_region_count; ++i) {
                const auto& region = text.content_hit_regions[i];
                if (region.semantic_index >= text.content_semantic_count)
                    continue;
                const auto& semantic = text.content_semantics[region.semantic_index];
                const bool flag = semantic.kind == MessageMarkdownSemanticKind::regional_flag;
                if (!(semantic.reserved & kRendererSearchSemanticFlag) || (semantic.reserved & 2u) ||
                    !semantic.target ||
                    (!flag && semantic.kind != MessageMarkdownSemanticKind::custom_emoji))
                    continue;
                const auto clip = ui_search_scene_content_clip(
                    *renderer->search_scene, {region.left, region.top, region.right, region.bottom});
                if (!ui_rect_has_area(clip))
                    continue;
                const auto allocation = media_atlas_peek(renderer->media_atlas, emoji_key(semantic));
                if (!allocation.ok() || renderer->media_quad_count >= kRendererMediaQuadCapacity)
                    continue;
                const u32 before = renderer->media_quad_count;
                if (!queue_media_quad(renderer, allocation, emoji_cell(region, flag), clip, false, first))
                    return layout_step_failed(__LINE__);
                if (renderer->media_quad_count != before)
                    first = false;
            }
        }
        if (!paint_chip_initial_plates(renderer, theme, true) || !queue_chip_guild_icons(renderer, true))
            return layout_step_failed(__LINE__);
    }
    if (d3d11_renderer_search_visible(*renderer) &&
        !split_paint_quad_boundary(&renderer->paint_stream, search_foreground_quad,
                                   &renderer->friends_requests_foreground_first_batch))
        return layout_step_failed(__LINE__);
    return true;
}

bool build_settings_page(D3D11Renderer* renderer, const Theme& theme) noexcept {
    // The shell beneath is the backdrop texture; keep what it shows resident.
    renderer_pin_settings_backdrop_media(renderer);
    renderer->paint_stream.filter_scrollbars = false;
    paint_stream_reset(&renderer->paint_stream);
    renderer->media_quad_count = 0;
    renderer->media_batch_count = 0;
    renderer->media_overlay_paint_first_batch = 0;
    renderer->profile_foreground_first_batch = 0;
    renderer->popup_media_first_batch = 0;
    renderer->menu_paint_first_batch = 0;
    renderer->menu_media_first_batch = 0;
    renderer->overlay_paint_first_batch = 0;
    renderer->edit_overlay_paint_first_batch = 0;
    renderer->form_paint_first_batch = 0;
    UiSettingsPaintRanges ranges{};
    // Split in ascending quad order so earlier batch indices stay valid.
    if (!ui_settings_scene_paint(*renderer->settings_scene, theme, &renderer->paint_stream,
                                 renderer->dpi, &ranges) ||
        !split_paint_quad_boundary(&renderer->paint_stream, ranges.modal_first_quad,
                                   &renderer->settings_modal_paint_first_batch) ||
        !split_paint_quad_boundary(&renderer->paint_stream, ranges.page_first_quad,
                                   &renderer->settings_page_paint_first_batch) ||
        !split_paint_quad_boundary(&renderer->paint_stream, ranges.page_end_quad,
                                   &renderer->settings_page_paint_end_batch) ||
        !split_paint_quad_boundary(&renderer->paint_stream, ranges.modal_end_quad,
                                   &renderer->settings_modal_paint_end_batch))
        return layout_step_failed(__LINE__);
    // Resident avatars replace their rows' loading placeholder, which a
    // transparent image must not show through; misses keep it.
    renderer->settings_page_media_first_batch = renderer->media_batch_count;
    if (renderer->media_atlas && renderer->media_gpu.texture_view) {
        UiSettingsAvatarGeometry avatar{};
        bool first = true;
        for (u32 cursor = 0; ui_settings_scene_next_avatar(*renderer->settings_scene, &cursor, &avatar);) {
            const auto allocation = media_atlas_peek(renderer->media_atlas, avatar.media->key);
            if (!allocation.ok())
                continue;
            const auto r = avatar.avatar;
            const u32 before = renderer->media_quad_count;
            // Emoji and sticker previews keep their own shape.
            const auto kind = avatar.media->key.kind;
            const bool round = kind != MediaAssetKind::custom_emoji && kind != MediaAssetKind::sticker;
            if (!queue_media_quad(renderer, allocation, r, avatar.clip, round,
                                  first))
                return layout_step_failed(__LINE__);
            reveal_media_underlay(*renderer, before, r, theme.avatar_placeholder);
            first = first && renderer->media_quad_count == before;
        }
    }
    renderer->settings_page_media_end_batch = renderer->media_batch_count;
    if (!queue_settings_profile_overlays(renderer, theme, false))
        return layout_step_failed(__LINE__);
    renderer->form_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    const auto* popup = d3d11_renderer_settings_popup(*renderer);
    renderer->settings_choice_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (popup && !popup_presentation_build(*popup, theme, &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    renderer->settings_choice_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
    if (!queue_settings_profile_overlays(renderer, theme, true))
        return layout_step_failed(__LINE__);
    renderer->form_paint_first_batch = paint_stream_seal(&renderer->paint_stream);
    if (d3d11_renderer_forum_form_active(*renderer) &&
        !forum_post_paint(*renderer->forum_form->scene, renderer->forum_form->geometry, theme,
                          &renderer->paint_stream))
        return layout_step_failed(__LINE__);
    renderer->forum_paint_end_batch = paint_stream_seal(&renderer->paint_stream);
    return (!d3d11_renderer_text_form_active(*renderer) ||
            text_form_paint(*renderer->text_form->scene, renderer->text_form->geometry, theme,
                            &renderer->paint_stream)) &&
           queue_toast(renderer, theme) && queue_inline_flags(renderer);
}

} // namespace renderer_layout_detail
} // namespace maicord
