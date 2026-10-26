#include "maicord/core/spsc_mailbox.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/render/raster_clip.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/channel_overview_controller.hpp"
#include "maicord/app/message_reaction_controller.hpp"
#include "maicord/app/poll_controller.hpp"
#include "maicord/app/voice_message.hpp"
#include "maicord/media/mp4_fragment.hpp"
#include "maicord/ui/download_scene.hpp"

namespace maicord {
namespace {

using Check = void (*)(bool, const char*) noexcept;

void test_composer_regressions(Check check) noexcept {
    char storage[128]{};
    MessageComposer composer{};
    check(message_composer_initialize(&composer, Slice<char>{storage, 128}), "composer: initialize");
    message_composer_reset(&composer, 1, false);
    message_composer_insert_utf8(&composer, utf8_literal("alpha beta\ngamma delta"));
    message_composer_set_selection(&composer, 2, 8);
    message_composer_move(&composer, MessageComposerMove::line_home, false);
    check(composer.caret == 0 && composer.anchor == 0, "composer: Home reaches line start with selection");
    message_composer_set_selection(&composer, 2, 8);
    message_composer_move(&composer, MessageComposerMove::line_end, false);
    check(composer.caret == 10 && composer.anchor == 10, "composer: End reaches line end with selection");
    message_composer_set_selection(&composer, 14, 18);
    message_composer_move(&composer, MessageComposerMove::document_home, false);
    check(composer.caret == 0, "composer: Ctrl+Home reaches document start with selection");
    message_composer_set_selection(&composer, 2, 8);
    message_composer_move(&composer, MessageComposerMove::document_end, false);
    check(composer.caret == composer.bytes, "composer: Ctrl+End reaches document end with selection");
    message_composer_set_selection(&composer, 8, 2);
    message_composer_move(&composer, MessageComposerMove::line_end, true);
    check(composer.anchor == 8 && composer.caret == 10, "composer: Shift+End retains anchor");
    message_composer_set_selection(&composer, 8, 2);
    message_composer_move(&composer, MessageComposerMove::previous, false);
    check(composer.anchor == 2 && composer.caret == 2, "composer: Left still collapses reversed selection");
    message_composer_set_selection(&composer, 8, 2);
    message_composer_move(&composer, MessageComposerMove::next, false);
    check(composer.anchor == 8 && composer.caret == 8,
          "composer: Right still collapses reversed selection");
    message_composer_set_selection(&composer, 21, 21);
    message_composer_move(&composer, MessageComposerMove::previous_word, false);
    check(composer.caret == 17, "composer: Ctrl+Left finds preceding word");
    message_composer_move(&composer, MessageComposerMove::previous_word, true);
    check(composer.anchor == 17 && composer.caret == 11, "composer: Ctrl+Shift+Left extends by word");
    check(utf8_equal(
              Utf8View{composer.text + minimum(composer.anchor, composer.caret),
                       maximum(composer.anchor, composer.caret) - minimum(composer.anchor, composer.caret)},
              utf8_literal("gamma ")),
          "composer: clipboard selection is ordered");
    message_composer_delete_word(&composer, false);
    check(utf8_equal(message_composer_text(composer), utf8_literal("alpha beta\ndelta")),
          "composer: word-delete removes existing selection only");
    message_composer_move(&composer, MessageComposerMove::document_home, false);
    message_composer_delete_word(&composer, true);
    check(utf8_equal(message_composer_text(composer), utf8_literal("beta\ndelta")),
          "composer: Ctrl+Delete removes next word and separator");
    message_composer_move(&composer, MessageComposerMove::document_end, false);
    message_composer_delete_word(&composer, false);
    check(utf8_equal(message_composer_text(composer), utf8_literal("beta\n")),
          "composer: Ctrl+Backspace removes preceding word");

    message_composer_clear(&composer);
    check(message_composer_insert_utf8(&composer, utf8_literal("\tcode\n\tline")) ==
              MessageComposerResult::changed,
          "composer: tab-indented clipboard text is accepted atomically");
    check(message_composer_can_send(composer, true), "composer: multiline code is sendable");
    message_composer_clear(&composer);
    message_composer_insert_utf8(&composer, utf8_literal(" \t\n\xC2\xA0\xE3\x80\x80"));
    check(!message_composer_can_send(composer, true), "composer: whitespace-only message is not sendable");
    message_composer_insert_scalar(&composer, 0x1F680);
    check(message_composer_can_send(composer, true, 7) && !message_composer_can_send(composer, true, 5),
          "composer: Unicode scalar limit and content validation share one pass");

    message_composer_reset(&composer, 2, false);
    message_composer_insert_utf8(&composer, utf8_literal("\xC5\xBEivjo \xF0\x9F\x9A\x80 svet"));
    message_composer_move(&composer, MessageComposerMove::previous_word, false);
    check(composer.caret == 12, "composer: word navigation preserves multibyte boundaries");
    message_composer_delete_word(&composer, false);
    check(utf8_equal(message_composer_text(composer), utf8_literal("\xC5\xBEivjo svet")),
          "composer: word deletion preserves complete emoji scalars");
    message_composer_set_selection(&composer, 1, 1);
    check(composer.caret == 7, "composer: selection cannot split a UTF-8 scalar");

    message_composer_reset(&composer, 3, false);
    message_composer_insert_utf8(&composer, utf8_literal("abcdef"));
    message_composer_set_selection(&composer, 1, 3);
    const u32 revision = composer.revision;
    check(message_composer_insert_utf8(&composer, Utf8View{storage + 3, 3}) ==
                  MessageComposerResult::invalid_input &&
              composer.revision == revision && composer.anchor == 1 && composer.caret == 3 &&
              utf8_equal(message_composer_text(composer), utf8_literal("abcdef")),
          "composer: aliased insertion is rejected without overwriting source or selection");
    check(message_composer_replace_range(&composer, 0, 1, Utf8View{storage + 3, 3}) ==
                  MessageComposerResult::invalid_input &&
              composer.anchor == 1 && composer.caret == 3,
          "composer: failed aliased range replacement restores selection");
    check(message_composer_insert_utf8(&composer, Utf8View{"\xF0\x9F", 2}) ==
                  MessageComposerResult::invalid_utf8 &&
              composer.revision == revision,
          "composer: truncated UTF-8 paste is atomic");
    check(message_composer_insert_utf8(&composer, Utf8View{"bad\0", 4}) ==
                  MessageComposerResult::invalid_input &&
              composer.revision == revision,
          "composer: disallowed control paste is atomic");
    message_composer_insert_utf16(&composer, 0xD83D);
    message_composer_reset(&composer, 4, true);
    check(message_composer_insert_utf16(&composer, 0xDE80) == MessageComposerResult::invalid_input &&
              composer.revision == revision,
          "composer: surrogate input cannot leak across channel reset");
    message_composer_clear(&composer);
    check(message_composer_delete_word(&composer, false) == MessageComposerResult::unchanged &&
              message_composer_delete_word(&composer, true) == MessageComposerResult::unchanged,
          "composer: word-delete at empty boundaries is a no-op");
    message_composer_insert_utf16(&composer, 0xD83D);
    message_composer_clear(&composer);
    check(message_composer_insert_utf16(&composer, 0xDE80) == MessageComposerResult::invalid_input,
          "composer: clearing an empty draft cancels an unfinished surrogate");
    message_composer_insert_utf8(&composer, utf8_literal("ab cd"));
    message_composer_move(&composer, MessageComposerMove::document_home, false);
    message_composer_move(&composer, MessageComposerMove::next_word, true);
    check(composer.anchor == 0 && composer.caret == 3,
          "composer: Ctrl+Shift+Right selects the word and separator");
    const u32 valid_bytes = composer.bytes;
    const u32 valid_revision = composer.revision;
    composer.bytes = composer.capacity + 1;
    check(message_composer_backspace(&composer) == MessageComposerResult::invalid_input &&
              message_composer_delete_forward(&composer) == MessageComposerResult::invalid_input &&
              message_composer_move(&composer, MessageComposerMove::next, false) ==
                  MessageComposerResult::invalid_input &&
              message_composer_set_selection(&composer, 0, 0) == MessageComposerResult::invalid_input &&
              message_composer_replace_range(&composer, 0, 0, utf8_literal("x")) ==
                  MessageComposerResult::invalid_input &&
              message_composer_clear(&composer) == MessageComposerResult::invalid_input &&
              composer.revision == valid_revision,
          "composer: corrupted length is rejected before any text access or mutation");
    composer.bytes = valid_bytes;
    char tiny_storage[4]{};
    MessageComposer tiny{};
    message_composer_initialize(&tiny, Slice<char>{tiny_storage, 4});
    message_composer_insert_utf8(&tiny, utf8_literal("abcd"));
    message_composer_set_selection(&tiny, 1, 2);
    const u32 tiny_revision = tiny.revision;
    check(message_composer_replace_range(&tiny, 1, 2, utf8_literal("too long")) ==
                  MessageComposerResult::capacity &&
              tiny.anchor == 1 && tiny.caret == 2 && tiny.revision == tiny_revision &&
              utf8_equal(message_composer_text(tiny), utf8_literal("abcd")),
          "composer: capacity failure preserves text, selection and revision");
}

void test_raster_regressions(Check check) noexcept {
    RasterClipRect clip = raster_clip_rectangle(UiRect{1, 2, 3, 5}, 1.25f, 100, 100);
    check(clip.left == 1 && clip.top == 2 && clip.right == 4 && clip.bottom == 7,
          "raster: 125 percent DPI conservatively includes trailing pixels");
    clip = raster_clip_rectangle(UiRect{0.1f, 0.1f, 0.2f, 0.2f}, 1.5f, 100, 100);
    check(clip.left == 0 && clip.top == 0 && clip.right == 1 && clip.bottom == 1,
          "raster: subpixel clip remains visible");
    clip = raster_clip_rectangle(UiRect{-100, -50, 300, 400}, 1.75f, 320, 240);
    check(clip.left == 0 && clip.top == 0 && clip.right == 320 && clip.bottom == 240,
          "raster: clip clamps to framebuffer before integer conversion");
    clip = raster_clip_rectangle(UiRect{0.5f, 0.5f, 0.5f, 10}, 1.25f, 100, 100);
    check(clip.right == 0 && clip.bottom == 0, "raster: empty DIP clip does not gain a pixel");
    clip = raster_clip_rectangle(UiRect{200, 0, 300, 20}, 1.0f, 100, 100);
    check(clip.right == 0 && clip.bottom == 0, "raster: wholly offscreen clip stays empty");
    clip = raster_clip_rectangle(UiRect{0, 0, 10, 10}, 0.0f, 100, 100);
    check(clip.right == 0 && clip.bottom == 0, "raster: zero scale rejected");
    bool contained = true;
    for (u32 scale = 96; scale <= 384; scale += 12) {
        for (u32 i = 1; i < 257; ++i) {
            const float left = static_cast<float>(i) / 7.0f;
            const float right = left + 0.25f;
            const float factor = static_cast<float>(scale) / 96.0f;
            clip = raster_clip_rectangle(UiRect{left, left, right, right}, factor, 1000, 1000);
            contained = contained && static_cast<float>(clip.left) <= left * factor &&
                        static_cast<float>(clip.right) >= right * factor && clip.right > clip.left;
        }
    }
    check(contained, "raster: 6400 fractional-DPI clip cases contain all touched pixels");
}

void test_mailbox_regressions(Check check) noexcept {
    // Deliberately non-power-of-two byte capacity: only descriptor capacity
    // is required to be a power of two by the shipping contract.
    SpscMailbox<u32, 4, 31> mailbox{};
    u8 payload[31]{};
    u8 output[33]{};
    bool exact = true;
    for (u32 offset = 0; offset < 31; ++offset) {
        for (u32 length = 0; length <= 31; ++length) {
            mailbox = SpscMailbox<u32, 4, 31>{};
            mailbox.payload_read_sequence = offset;
            mailbox.payload_write_sequence = offset;
            for (u32 i = 0; i < length; ++i)
                payload[i] = static_cast<u8>(i * 17u + offset);
            output[0] = 0xA5;
            output[length + 1] = 0x5A;
            u32 value = 0;
            const SpscMailboxResult pushed =
                spsc_mailbox_push(&mailbox, 123u, ConstSlice<u8>{payload, length});
            const SpscMailboxResult popped =
                spsc_mailbox_pop(&mailbox, &value, Slice<u8>{output + 1, length});
            exact = exact && pushed.ok() && popped.ok() && popped.payload_bytes == length && value == 123 &&
                    bytes_equal(output + 1, payload, length) && output[0] == 0xA5 &&
                    output[length + 1] == 0x5A;
        }
    }
    check(exact, "mailbox: 992 ring offset/length combinations preserve payload and guards");
    mailbox = SpscMailbox<u32, 4, 31>{};
    mailbox.read_sequence = static_cast<LONG>(0xFFFFFFFEu);
    mailbox.write_sequence = static_cast<LONG>(0xFFFFFFFEu);
    bool wrap = true;
    for (u32 i = 0; i < 20; ++i) {
        u32 value = ~0u;
        wrap = wrap && spsc_mailbox_push(&mailbox, i, ConstSlice<u8>{payload, 13}).ok() &&
               spsc_mailbox_pop(&mailbox, &value, Slice<u8>{output, 13}).ok() && value == i;
    }
    check(wrap && spsc_mailbox_size(mailbox) == 0, "mailbox: 32-bit descriptor sequence wraps safely");
    spsc_mailbox_push(&mailbox, 17u, ConstSlice<u8>{payload, 5});
    u32 value = 0;
    const SpscMailboxResult too_small = spsc_mailbox_pop(&mailbox, &value, Slice<u8>{output, 4});
    const SpscMailboxResult discarded = spsc_mailbox_discard(&mailbox, &value);
    check(too_small.error == SpscMailboxError::output_too_small && too_small.payload_bytes == 5 &&
              discarded.ok() && discarded.payload_bytes == 5 && value == 17 &&
              spsc_mailbox_size(mailbox) == 0,
          "mailbox: failed small output retains ownership until discard");

    // External records keep their payload outside the ring, in FIFO order
    // with ordinary records, and never move the ring's byte sequences.
    mailbox = SpscMailbox<u32, 4, 31>{};
    u8 outside[64]{};
    for (u32 i = 0; i < 64; ++i)
        outside[i] = static_cast<u8>(i * 3u + 1u);
    u8 large_output[64]{};
    const bool queued = spsc_mailbox_push(&mailbox, 1u, ConstSlice<u8>{payload, 7}).ok() &&
                        spsc_mailbox_push_external(&mailbox, 2u, 64).external &&
                        spsc_mailbox_push(&mailbox, 3u, ConstSlice<u8>{payload, 9}).ok() &&
                        !spsc_mailbox_push_external(&mailbox, 9u, 0).ok();
    u32 first = 0, second = 0, third = 0;
    const SpscMailboxResult ordinary = spsc_mailbox_pop(&mailbox, &first, Slice<u8>{output, 7});
    const bool head = spsc_mailbox_head_external(mailbox);
    const u64 ring_read = static_cast<u64>(mailbox.payload_read_sequence);
    const SpscMailboxResult missing = spsc_mailbox_pop(&mailbox, &second, Slice<u8>{large_output, 64});
    const SpscMailboxResult small = spsc_mailbox_pop(&mailbox, &second, Slice<u8>{large_output, 63},
                                                     ConstSlice<u8>{outside, 64});
    const SpscMailboxResult copied = spsc_mailbox_pop(&mailbox, &second, Slice<u8>{large_output, 64},
                                                      ConstSlice<u8>{outside, 64});
    const u64 ring_after_external = static_cast<u64>(mailbox.payload_read_sequence);
    const SpscMailboxResult after = spsc_mailbox_pop(&mailbox, &third, Slice<u8>{output, 9});
    check(queued && ordinary.ok() && !ordinary.external && first == 1 && head &&
              missing.error == SpscMailboxError::corrupt && missing.external &&
              small.error == SpscMailboxError::output_too_small && small.payload_bytes == 64 &&
              copied.ok() && copied.external && second == 2 && bytes_equal(large_output, outside, 64) &&
              ring_after_external == ring_read && after.ok() && third == 3 &&
              bytes_equal(output, payload, 9) && !spsc_mailbox_head_external(mailbox) &&
              spsc_mailbox_size(mailbox) == 0 && spsc_mailbox_payload_used(mailbox) == 0,
          "mailbox: an external record is copied from its producer buffer in FIFO order outside the ring");
    u32 dropped = 0;
    const bool discarded_external = spsc_mailbox_push_external(&mailbox, 5u, 40).ok() &&
                                    spsc_mailbox_discard(&mailbox, &dropped).external && dropped == 5 &&
                                    spsc_mailbox_payload_used(mailbox) == 0 && spsc_mailbox_size(mailbox) == 0;
    check(discarded_external, "mailbox: discarding an external record releases only its descriptor");
}

// reset_value clears the bytes of a kZeroDefault type: its value-initialized form must be all zero bytes.
template <typename T> bool zero_default_holds() noexcept {
    static_assert(requires { T::kZeroDefault; });
    alignas(T) static u8 storage[sizeof(T)];
    clear_bytes(storage, static_cast<u32>(sizeof(T)));
    ::new (static_cast<void*>(storage)) T{};
    return bytes_zero(storage, sizeof(T));
}
void test_zero_default_types(Check check) noexcept {
    check(zero_default_holds<ChannelOverviewController>(), "zero default: channel overview controller");
    check(zero_default_holds<MessageReactionController>(), "zero default: message reaction controller");
    check(zero_default_holds<PollController>(), "zero default: poll controller");
    check(zero_default_holds<VoiceMessageRecorder>(), "zero default: voice message recorder");
    check(zero_default_holds<Mp4MediaSegment>(), "zero default: mp4 media segment");
    check(zero_default_holds<UiDownloadScene>(), "zero default: download scene");
}

} // namespace

void test_mailbox_release_races(Check check) noexcept;
void test_responsiveness(Check check) noexcept;

void test_engineering_regressions(Check check) noexcept {
    test_composer_regressions(check);
    test_raster_regressions(check);
    test_mailbox_regressions(check);
    test_mailbox_release_races(check);
    test_responsiveness(check);
    test_zero_default_types(check);
    MessageComposer owned{};
    check(message_composer_initialize_owned(&owned, 128), "composer regression: initialize owned history");
    if (owned.text) {
        message_composer_restore(&owned, 1, utf8_literal("x<@123>y"), 3, 6);
        const MessageComposerAtomicSpan span{1, 7};
        owned.atomic_spans = {&span, 1};
        owned.atomic_revision = owned.text_revision;
        check(message_composer_insert_utf8(&owned, utf8_literal("\tcode")) ==
                      MessageComposerResult::changed &&
                  utf8_equal(message_composer_text(owned), utf8_literal("x\tcodey")) &&
                  message_composer_undo(&owned) == MessageComposerResult::changed &&
                  utf8_equal(message_composer_text(owned), utf8_literal("x<@123>y")) && owned.anchor == 3 &&
                  owned.caret == 6 && message_composer_redo(&owned) == MessageComposerResult::changed &&
                  utf8_equal(message_composer_text(owned), utf8_literal("x\tcodey")),
              "composer tab replacement keeps atomic token removal and undo/redo selection");
        const auto history = message_composer_history_costs(owned);
        check(message_composer_insert_utf8(&owned, {owned.text, 2}) ==
                      MessageComposerResult::invalid_input &&
                  message_composer_history_costs(owned).undo_steps == history.undo_steps,
              "composer rejected borrowed insertion does not append undo history");
        message_composer_release(&owned);
    }
}

} // namespace maicord
