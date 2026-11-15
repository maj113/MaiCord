#include "embed_parity_internal.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/theme/background_presentation.hpp"

namespace maicord {

// A video that this computer has no decoder for (an MP4 attachment, H.264, and a Tenor GIFV embed, HEVC): the media
// pump marks each item, and the box then keeps its size and fill, loses its play control and shows a centred label
// in the muted text colour of a blocked notice. An item without a mark draws as before.
void test_unplayable_video(Check check) noexcept {
    auto& f = fixture;
    const auto payload = utf8_literal(R"json({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Poster"},"content":"","timestamp":"2026-10-09T17:00:00Z","attachments":[{"id":"70","filename":"clip.mp4","size":4096,"url":"https://cdn.discordapp.com/attachments/7/70/clip.mp4","proxy_url":"https://media.discordapp.net/attachments/7/70/clip.mp4","content_type":"video/mp4","width":320,"height":180}],"embeds":[{"type":"gifv","url":"https://tenor.com/view/cat-123","provider":{"name":"Tenor","url":"https://tenor.co"},"thumbnail":{"url":"https://media.tenor.com/cat.png","proxy_url":"https://images-ext-1.discordapp.net/external/a/https/media.tenor.com/cat.png","width":220,"height":220},"video":{"url":"https://media.tenor.com/cat.mp4","proxy_url":"https://images-ext-1.discordapp.net/external/b/https/media.tenor.com/cat.mp4","width":220,"height":220}}]}})json");
    GatewayMessageEvent message{};
    if (!decode_message(payload, &message) || !initialize_fixture(message, 4000)) {
        check(false, "unplayable_video_fixture_initializes");
        return;
    }
    ChatStateStore state{};
    NavigationView navigation{};
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr, &navigation,
                                                &kDefaultTheme, finish, nullptr, 1'791'600'000, 1};
    MessageMediaCommandStream media{};
    MessagePresentationResult painted{};
    const auto repaint = [&]() noexcept {
        return message_scene_update(&f.scene, {0, 0, 900, 4000}).ok() &&
               message_text_coordinator_converge(request).ok() && paint_stream_reset(&f.paint, 900, 4000) &&
               message_media_command_stream_initialize(&media, {f.media, 32}) &&
               message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &painted);
    };
    struct Shot {
        UiRect attachment, embed, row;
        bool play;
        u32 quads, fills;
    };
    const Utf8View no_decoder = utf8_literal("No decoder for this video format");
#if defined(_WIN32)
    const Utf8View install = no_decoder;
#else
    const Utf8View install = utf8_literal("Install OpenH264 to play this video");
#endif
    const auto shot = [&](Shot* s) noexcept {
        *s = {};
        for (u32 i = 0; i < media.count; ++i) {
            const auto& c = media.storage[i];
            if (c.kind == MessageMediaCommandKind::attachment) {
                s->attachment = c.rectangle;
                s->play = c.ordinary_video;
            } else if (c.kind == MessageMediaCommandKind::embed) {
                s->embed = c.rectangle;
            }
        }
        const auto* row = message_scene_find_row(f.scene, 0);
        const auto* node = row ? message_scene_layout_node(f.scene, row->widget) : nullptr;
        s->row = node ? node->rectangle : UiRect{};
        s->quads = painted.geometry_quads;
        // The boxes keep the fill the preview had: the theme's surface_high.
        for (u32 i = 0; i < f.paint.count; ++i) {
            const auto& q = f.paint.quads[i];
            const UiRect box{q.left, q.top, q.right, q.bottom};
            const bool fill = q.fill.red == kDefaultTheme.surface_high.red &&
                              q.fill.green == kDefaultTheme.surface_high.green &&
                              q.fill.blue == kDefaultTheme.surface_high.blue &&
                              q.fill.alpha == kDefaultTheme.surface_high.alpha;
            s->fills += fill && ((ui_rect_equal(box, s->attachment) && q.radius == kDefaultTheme.scale.radius_md) ||
                                 (ui_rect_equal(box, s->embed) && q.radius == 4.0f));
        }
        return ui_rect_has_area(s->attachment) && ui_rect_has_area(s->embed) && ui_rect_has_area(s->row);
    };
    const auto label = [&](Utf8View text, u32 nth = 0) noexcept -> const MessageTextCommand* {
        for (u32 i = 0; i < f.presentation.command_count; ++i)
            if (utf8_equal(f.presentation.commands[i].text, text) && !nth--)
                return &f.presentation.commands[i];
        return nullptr;
    };
    Shot before{}, after{};
    check(repaint() && shot(&before) && before.play && before.fills == 2 && !label(no_decoder) && !label(install),
          "unplayable_video_unmarked_items_draw_as_before");
    const u64 attachment = message_media_reveal_key(1, MessageMediaSource::attachment, 0);
    const u64 embed = message_media_reveal_key(1, MessageMediaSource::embed, 0);
    check(message_scene_mark_unplayable(&f.scene, attachment, MessageMediaUnplayable::h264) &&
              message_scene_mark_unplayable(&f.scene, embed, MessageMediaUnplayable::hevc) &&
              !message_scene_mark_unplayable(&f.scene, attachment, MessageMediaUnplayable::h264) &&
              !message_scene_mark_unplayable(&f.scene, 7, MessageMediaUnplayable::none),
          "unplayable_video_marks_each_item_once");
    check(utf8_equal(message_media_unplayable_label(MessageMediaUnplayable::h264), install) &&
              utf8_equal(message_media_unplayable_label(MessageMediaUnplayable::hevc), no_decoder) &&
              !message_media_unplayable_label(MessageMediaUnplayable::none).bytes,
          "unplayable_video_label_names_what_to_install_or_the_missing_decoder");
    const bool painted_again = repaint() && shot(&after);
    check(painted_again && ui_rect_equal(after.attachment, before.attachment) &&
              ui_rect_equal(after.embed, before.embed) && ui_rect_equal(after.row, before.row) &&
              after.quads == before.quads && after.fills == 2,
          "unplayable_video_box_keeps_its_size_and_fill");
    check(painted_again && !after.play, "unplayable_video_attachment_drops_its_play_control");
    const Color muted = ui_background_muted_text_color(kDefaultTheme);
    const auto notice = [&](const MessageTextCommand* c, const UiRect& box) noexcept {
        return c && c->color.red == muted.red && c->color.green == muted.green && c->color.blue == muted.blue &&
               c->color.alpha == muted.alpha && !c->styles.count &&
               c->horizontal_alignment == UiTextAlignment::center &&
               c->vertical_alignment == UiTextAlignment::center && c->wrapping == TextWrapping::no_wrap &&
               c->overflow == TextOverflow::ellipsis && close_to(c->rectangle.left, box.left + 12.0f) &&
               close_to(c->rectangle.right, box.right - 12.0f) && close_to(c->rectangle.top, box.top) &&
               close_to(c->rectangle.bottom, box.bottom) && c->measured_height > 0;
    };
#if defined(_WIN32)
    // Both items use the no-decoder label here: the attachment's comes first.
    const MessageTextCommand* attachment_label = label(no_decoder, 0);
    const MessageTextCommand* embed_label = label(no_decoder, 1);
#else
    const MessageTextCommand* attachment_label = label(install);
    const MessageTextCommand* embed_label = label(no_decoder);
#endif
    check(painted_again && notice(attachment_label, after.attachment),
          "unplayable_video_attachment_label_is_centred_muted_text_across_the_box");
    check(painted_again && notice(embed_label, after.embed),
          "unplayable_video_gifv_label_is_centred_muted_text_across_the_box");
    f.scene.media_unplayable = {};
    message_scene_release(&f.scene);
}

} // namespace maicord
