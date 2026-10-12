#include "maicord/text/bounded_edit.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
struct ReadyFixture { BoundedEdit* edit; u64 observed; bool allowed, rebind, rebind_same; };
bool host_ready(void* context, u64 identity) noexcept {
    auto& fixture = *static_cast<ReadyFixture*>(context);
    fixture.observed = identity;
    if (fixture.rebind) fixture.edit->identity = identity + 1;
    if (fixture.rebind_same) {
        fixture.rebind_same=false;
        const auto binding=*fixture.edit;
        bounded_edit_bind(fixture.edit,binding.composer,binding.maximum_codepoints,binding.multiline,
                          binding.identity,binding.ready,binding.context);
    }
    return fixture.allowed;
}
}
void test_bounded_edit(void (*check)(bool, const char*) noexcept) noexcept {
    char storage[64]{};
    MessageComposer model{};
    BoundedEdit edit{};
    check(message_composer_initialize(&model, {storage, 64}) &&
        bounded_edit_bind(&edit, &model, 3, false, 10) && !model.history,
        "bounded edit: borrowed empty name binds without retained history allocation");
    auto target = bounded_edit_input_target(&edit);
    TextInputChange change{9, 9, 9};
    const u16 original[]{'a', 0xD83D, 0xDE80, 'b'};
    check(text_input_replace(target, 0, 0, {original, 4}, &change) == TextInputResult::changed &&
        message_composer_codepoints(model) == 3 && model.bytes == 6 && change.new_end == 4,
        "bounded edit: supplementary scalar counts once while native positions count two UTF16 units");
    const u16 extra[]{'x'};
    const u32 revision = model.text_revision;
    change = {9, 9, 9};
    check(text_input_replace(target, 4, 4, {extra, 1}, &change) == TextInputResult::capacity &&
        model.text_revision == revision && model.bytes == 6 && change.start == 9,
        "bounded edit: overlimit insertion is atomic and does not publish change output");
    const u16 bad_controls[]{'x', '\n', 'y'};
    check(text_input_replace(target, 0, 4, {bad_controls, 3}, &change) == TextInputResult::invalid_input &&
        model.text_revision == revision && model.bytes == 6,
        "bounded edit: name newline rejects whole replacement without deleting old value");
    const u16 bad_surrogate[]{0xD83D, 'x'};
    check(text_input_replace(target, 0, 4, {bad_surrogate, 2}, &change) == TextInputResult::invalid_utf16 &&
        text_input_replace(target, 1, 2, {extra, 1}, &change) == TextInputResult::invalid_utf16 &&
        model.text_revision == revision,
        "bounded edit: malformed input and split surrogate range cannot mutate document");
    check(text_input_select(target, {1, 3}) == TextInputResult::changed,
        "bounded edit: delegates Unicode-safe selection");
    u16 copied[4]{}; u32 count = 0, next = 0;
    check(text_input_copy(target, 1, 3, {copied, 4}, &count, &next) == TextInputResult::unchanged &&
        count == 2 && next == 3 && copied[0] == 0xD83D && copied[1] == 0xDE80,
        "bounded edit: delegates scalar-preserving native copy");
    check(text_input_begin_group(target) && model.history &&
        message_composer_history_costs(model).payload_bytes == 2048,
        "bounded edit: first grouped edit lazily enables bounded history");
    check(text_input_replace(target, 0, 4, {}, &change) == TextInputResult::changed && model.bytes == 0,
        "bounded edit: transient empty names remain editable");
    text_input_end_group(target);
    check(message_composer_undo(&model) == MessageComposerResult::changed && model.bytes == 6 &&
        message_composer_redo(&model) == MessageComposerResult::changed && model.bytes == 0,
        "bounded edit: normal undo and redo retain grouped draft edits");
    check(bounded_edit_bind(&edit, &model, 3, true, 11),
        "bounded edit: topic binding uses separate host scope");
    target = bounded_edit_input_target(&edit);
    const u16 topic[]{'a', '\n', 'b'};
    check(text_input_replace(target, 0, 0, {topic, 3}, &change) == TextInputResult::changed &&
        utf8_equal(message_composer_text(model), utf8_literal("a\nb")),
        "bounded edit: multiline topic accepts ordinary linefeeds");
    const u16 rejected_controls[]{0, 9, 13, 127, 159};
    for (const u16 control : rejected_controls)
        check(text_input_replace(target, 0, 3, {&control, 1}, &change) == TextInputResult::invalid_input &&
            model.bytes == 3, "bounded edit: control-character replacement stays atomic");
    const auto old_target = target;
    model.anchor = 1; model.caret = 60; model.pending_high_surrogate = 0xD83D;
    const u64 document = model.document_identity;
    copy_bytes(storage, "\xF0\x9F\x9A\x80z", 5);
    storage[63] = 'q';
    check(message_composer_refresh_borrowed(&model, 5) && model.bytes == 5 && model.anchor == 0 &&
        model.caret == 5 && !model.pending_high_surrogate && model.document_identity != document &&
        storage[0] == static_cast<char>(0xF0) && storage[63] == 'q' &&
        message_composer_undo(&model) == MessageComposerResult::unchanged && !text_input_ready(old_target),
        "bounded edit: external refresh keeps borrowed bytes, clamps boundaries and retires history identity");
    check(bounded_edit_bind(&edit, &model, 3, false, 11) && !text_input_ready(old_target) &&
        edit.identity == 11 && text_input_ready(bounded_edit_input_target(&edit)),
        "bounded edit: refreshed document rebound under identical host scope cannot revive old native target");
    target = bounded_edit_input_target(&edit);
    const u64 same_document = model.document_identity;
    check(bounded_edit_bind(&edit, &model, 3, false, 11) && model.document_identity == same_document &&
        !text_input_ready(target) && bounded_edit_input_target(&edit).identity > target.identity &&
        text_input_ready(bounded_edit_input_target(&edit)),
        "bounded edit: even identical-document rebinding expires prior native binding ticket");
    target = bounded_edit_input_target(&edit);
    const u64 accepted_document = model.document_identity;
    const u32 accepted_revision = model.text_revision;
    check(!message_composer_refresh_borrowed(&model, 65) &&
        model.document_identity == accepted_document && model.text_revision == accepted_revision,
        "bounded edit: capacity rejection leaves model metadata unchanged");
    storage[0] = static_cast<char>(0xFF);
    check(!message_composer_refresh_borrowed(&model, 5) && model.document_identity == accepted_document,
        "bounded edit: malformed external UTF8 cannot publish a replacement document");
    storage[0] = static_cast<char>(0xF0);
    model.owns_storage = true;
    check(!message_composer_refresh_borrowed(&model, 5),
        "bounded edit: borrowed refresh cannot adopt an owned allocation");
    model.owns_storage = false;
    ReadyFixture fixture{&edit, 0, false, false};
    check(bounded_edit_bind(&edit, &model, 3, false, 20, host_ready, &fixture) &&
        !text_input_ready(bounded_edit_input_target(&edit)) && fixture.observed == 20,
        "bounded edit: host readiness gates native input with copied scope");
    fixture.allowed=true; fixture.rebind_same=true;
    target=bounded_edit_input_target(&edit);
    check(!text_input_ready(target) && fixture.observed==20 && edit.identity==20 &&
        bounded_edit_input_target(&edit).identity>target.identity &&
        text_input_ready(bounded_edit_input_target(&edit)),
        "bounded edit: callback same-scope rebind cannot authorize old native ticket but successor remains usable");
    fixture.rebind = true;
    target = bounded_edit_input_target(&edit);
    check(!text_input_ready(target) && fixture.observed == 20 && edit.identity == 21,
        "bounded edit: callback reentry cannot authorize its successor under old target identity");
    check(!bounded_edit_bind(&edit, &model, 0, false, 30) && edit.identity == 21,
        "bounded edit: invalid binding leaves prior descriptor unchanged");
    BoundedEdit exhausted{};
    check(bounded_edit_bind(&exhausted, &model, 3, false, 31),
        "bounded edit: overflow fixture initializes valid ordinary binding");
    exhausted.binding_identity=~u64{0};
    check(!bounded_edit_bind(&exhausted, &model, 3, false, 32) &&
        exhausted.binding_identity==~u64{0} && exhausted.identity==31,
        "bounded edit: exhausted native binding ticket rejects replacement without wrapping or mutation");
    message_composer_release(&model);

    char topic_storage[4096]{};
    MessageComposer topic_model{}; BoundedEdit topic_edit{};
    check(message_composer_initialize(&topic_model, {topic_storage, 4096}) &&
        bounded_edit_bind(&topic_edit, &topic_model, 1024, true, 40) && !topic_model.history,
        "bounded edit: full Unicode topic capacity keeps history lazy");
    const auto topic_target = bounded_edit_input_target(&topic_edit);
    check(text_input_begin_group(topic_target) && topic_model.history &&
        message_composer_history_costs(topic_model).payload_bytes == 16 * 1024 &&
        message_composer_history_costs(topic_model).group_bytes == 4096 &&
        text_input_replace(topic_target, 0, 0, {topic, 3}, &change) == TextInputResult::changed,
        "bounded edit: large topic admits bounded 16-KiB payload and one 4-KiB group snapshot");
    text_input_end_group(topic_target);
    check(message_composer_undo(&topic_model) == MessageComposerResult::changed && topic_model.bytes == 0 &&
        message_composer_redo(&topic_model) == MessageComposerResult::changed && topic_model.bytes == 3,
        "bounded edit: topic-sized borrowed documents retain working grouped undo and redo");
    message_composer_release(&topic_model);

    constexpr auto existing_topic=utf8_literal("a\t\r\xC2\x85" "b");
    check(message_composer_initialize(&model,{storage,64}),
        "bounded edit: authoritative compatibility fixture initializes borrowed buffer");
    copy_bytes(storage,existing_topic.data,existing_topic.bytes);
    check(message_composer_refresh_borrowed(&model,existing_topic.bytes) &&
        bounded_edit_bind(&edit,&model,16,true,50),
        "bounded edit: existing server TAB CR and C1 topic remains bindable without normalization");
    target=bounded_edit_input_target(&edit); count=next=0;
    u16 existing_copy[8]{};
    check(text_input_copy(target,0,5,{existing_copy,8},&count,&next)==TextInputResult::unchanged &&
        count==5 && existing_copy[1]=='\t' && existing_copy[2]=='\r' && existing_copy[3]==0x85 &&
        text_input_select(target,{1,4})==TextInputResult::changed &&
        text_input_replace(target,1,4,{},&change)==TextInputResult::changed &&
        utf8_equal(message_composer_text(model),utf8_literal("ab")),
        "bounded edit: existing topic controls support native copy selection and atomic removal");
    const u16 new_tab[]{'\t'};
    check(text_input_replace(target,1,1,{new_tab,1},&change)==TextInputResult::invalid_input &&
        utf8_equal(message_composer_text(model),utf8_literal("ab")),
        "bounded edit: existing control compatibility does not weaken new topic insertion policy");
    constexpr auto existing_name=utf8_literal("x\xE2\x80\xA8\xE2\x80\xA9" "y");
    copy_bytes(storage,existing_name.data,existing_name.bytes);
    check(message_composer_refresh_borrowed(&model,existing_name.bytes) &&
        bounded_edit_bind(&edit,&model,16,false,51),
        "bounded edit: authoritative Unicode line separators in name bind without changing bytes");
    target=bounded_edit_input_target(&edit); count=next=0;
    check(text_input_copy(target,0,4,{existing_copy,8},&count,&next)==TextInputResult::unchanged &&
        count==4 && existing_copy[1]==0x2028 && existing_copy[2]==0x2029 &&
        text_input_select(target,{1,3})==TextInputResult::changed &&
        text_input_replace(target,1,3,{},&change)==TextInputResult::changed &&
        utf8_equal(message_composer_text(model),utf8_literal("xy")),
        "bounded edit: authoritative name separators remain copyable selectable and removable");
    const u16 new_separator[]{0x2028};
    check(text_input_replace(target,1,1,{new_separator,1},&change)==TextInputResult::invalid_input &&
        utf8_equal(message_composer_text(model),utf8_literal("xy")),
        "bounded edit: new name separator insertion stays forbidden");
    storage[0]=0;
    check(message_composer_refresh_borrowed(&model,1) && !bounded_edit_bind(&edit,&model,16,true,52),
        "bounded edit: existing document compatibility still rejects embedded NUL");
    message_composer_release(&model);
}
} // namespace maicord
