#pragma once
#include "forum_media_ui_adapter.hpp"
#include "application_forum_form_state.hpp"
#include "application_forum_preview.hpp"
#include "maicord/core/log.hpp"
namespace maicord {
template <class Begin, class Resize, class Show, class Warm, class Metadata, class Type, class Fade>
bool forum_media_ui_scenarios(D3D11Renderer& renderer, Theme& theme, Begin begin, Resize resize, Show show,
                              Warm warm, Metadata metadata, Type type, Fade fade) noexcept {
    using namespace app_detail;
    const auto require = [](bool value, u32 stage) noexcept {
        if (!value)
            log_u64(LogLevel::error, "probe", "forum_media_native_failure_stage", stage);
        return value;
    };
    if (!require(begin() && resize(1100, 1000, 96) && forum_media_ui_adapter_enable() &&
                     forum_post_ui_adapter_open() && metadata(91001, false) && type(),
                 1))
        return false;
    if (!require(show(L"forum-media-empty-wide.bmp"), 2))
        return false;
    if (!require(forum_media_ui_adapter_seed_drop(), 301) ||
        !require(show(L"forum-media-preparing.bmp"), 302))
        return false;
    const auto ticket = application_forum_post_view().ticket;
    const u64 deadline = GetTickCount64() + 10000;
    while (!application_forum_attachments_view(ticket).rows.count && GetTickCount64() < deadline) {
        if (!forum_media_ui_adapter_pump())
            return false;
        Sleep(1);
    }
    const auto files = application_forum_attachments_view(ticket);
    if (!require(files.rows.count == 1 && forum_media_ui_adapter_drop_consumed() &&
                     forum_post_ui_adapter_synchronize(),
                 4))
        return false;
    const u64 id = files.rows[0].id, bytes = files.rows[0].bytes;
    if (!require(forum_media_ui_adapter_paste_file(), 401))
        return false;
    const u64 paste_deadline = GetTickCount64() + 10000;
    while (application_forum_attachments_view(ticket).rows.count < 2 && GetTickCount64() < paste_deadline) {
        if (!forum_media_ui_adapter_pump())
            return false;
        Sleep(1);
    }
    const auto pasted = application_forum_attachments_view(ticket).rows;
    if (!require(pasted.count == 2 && pasted[0].id == id && pasted[1].id != id &&
                     forum_post_ui_adapter_pointer({ForumPostElement::attachments,
                                                    0,
                                                    pasted[1].id,
                                                    UiAttachmentDraftAction::remove,
                                                    {}}) &&
                     application_forum_attachments_view(ticket).rows.count == 1,
                 402))
        return false;
    log_write(LogLevel::info, "probe",
              "forum_media_injected_clipboard_source_native_marshal_worker_filter_ownership_keyboard_"
              "restore_and_os_sequence_unchanged_passed");
    const auto action = [&](UiAttachmentDraftAction value) noexcept {
        return forum_post_ui_adapter_pointer({ForumPostElement::attachments, 0, id, value, {}});
    };
    if (!require(action(UiAttachmentDraftAction::spoiler) && show(L"forum-media-spoiler.bmp") &&
                     action(UiAttachmentDraftAction::spoiler),
                 5))
        return false;
    if (!require(action(UiAttachmentDraftAction::describe) && forum_media_ui_adapter_child(), 6))
        return false;
    auto* parent = application_forum_form_state();
    if (!require(parent && parent->child_suspended && !parent->blink_at_ms && !parent->caret_visible &&
                     !secure_text_store_attached(parent->fields[0].store) &&
                     !secure_text_store_attached(parent->fields[1].store) &&
                     show(L"forum-media-description-child.bmp"),
                 7))
        return false;
    const u32 description[]{'O', 'r', 'i', 'g', 'i', 'n', 'a', 'l', ' ', 'p', 'h', 'o', 't',
                            'o', ' ', 'd', 'e', 's', 'c', 'r', 'i', 'p', 't', 'i', 'o', 'n'};
    for (u32 unit : description)
        if (!forum_post_ui_adapter_input(UiInputKind::character, unit))
            return false;
    if (!require(
            forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN, false, true) &&
                !forum_media_ui_adapter_child() &&
                forum_media_ui_adapter_description_saved(id, utf8_literal("Original photo description")) &&
                !parent->child_suspended,
            8))
        return false;
    // URL and PUT are typed offline callbacks. The source intake and preview
    // decode below use the shipping native worker and the real preview atlas.
    Utf8View url{};
    if (!require(forum_post_ui_adapter_submit(91002) && forum_media_ui_adapter_url(&url) &&
                     forum_post_ui_adapter_response(200, url) &&
                     forum_media_ui_adapter_upload(bytes / 2, false) && show(L"forum-media-uploading.bmp"),
                 9))
        return false;
    if (!require(forum_media_ui_adapter_upload(bytes / 2, true, true) && show(L"forum-media-failed.bmp"),
                 10))
        return false;
    if (!require(action(UiAttachmentDraftAction::retry) && forum_post_ui_adapter_submit(91003) &&
                     forum_media_ui_adapter_url(&url) && forum_post_ui_adapter_response(200, url) &&
                     forum_media_ui_adapter_upload(bytes, true),
                 11))
        return false;
    const u64 preview_deadline = GetTickCount64() + 10000;
    while (!forum_media_ui_adapter_preview_ready() && GetTickCount64() < preview_deadline) {
        if (!forum_media_ui_adapter_pump())
            return false;
        Sleep(1);
    }
    if (!require(forum_media_ui_adapter_preview_ready() && action(UiAttachmentDraftAction::describe) &&
                     forum_media_ui_adapter_child(),
                 12))
        return false;
    if (!require(forum_post_ui_adapter_input(UiInputKind::key_down, VK_ESCAPE) &&
                     !forum_media_ui_adapter_child() && action(UiAttachmentDraftAction::spoiler) &&
                     action(UiAttachmentDraftAction::spoiler) && show(L"forum-media-ready-thumbnail.bmp") &&
                     renderer.forum_media_first_batch < renderer.forum_media_end_batch && fade() && warm(1),
                 13))
        return false;
    theme = kLightTheme;
    if (!require(resize(480, 1050, 144) && forum_media_ui_adapter_reveal_narrow(id) &&
                     show(L"forum-media-ready-narrow-light-144.bmp") && warm(2),
                 14))
        return false;
    theme = kDefaultTheme;
    ForumPostRequest create{};
    u64 create_id = 0;
    if (!require(
            resize(1100, 1000, 96) && forum_post_ui_adapter_pointer({ForumPostElement::body, 0}) &&
                forum_post_ui_adapter_input(UiInputKind::key_down, 'A', false, true) &&
                forum_post_ui_adapter_input(UiInputKind::key_down, VK_BACK) &&
                forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN, false, true) &&
                forum_post_ui_adapter_submit(91004) && forum_post_ui_adapter_flight(&create, &create_id) &&
                create.operation == ForumPostOperation::create && create.attachments.count == 1 &&
                !create.content.bytes &&
                utf8_equal(create.attachments[0].description, utf8_literal("Original photo description")) &&
                !application_forum_attachments_view(ticket).rows[0].can_remove &&
                !application_forum_form_state()->editable && forum_post_ui_adapter_response(400),
            15))
        return false;
    log_write(LogLevel::info, "probe",
              "forum_media_actual_attachment_only_create_immutable_references_passed");
    if (!require(action(UiAttachmentDraftAction::remove) &&
                     !application_forum_attachments_current(ticket, id) &&
                     application_forum_attachments_view(ticket).rows.count == 0 &&
                     show(L"forum-media-removed.bmp"),
                 16))
        return false;
    log_write(LogLevel::info, "probe",
              "forum_media_actual_native_drop_two_fields_child_description_retry_spoiler_remove_and_worker_"
              "thumbnail_passed");
    log_write(LogLevel::info, "probe",
              "forum_media_fixture_boundary_typed_offline_url_put_with_actual_native_source_worker_png_"
              "decode_and_preview_atlas_with_injected_clipboard_source_live_os_clipboard_not_proven");
    return true;
}
} // namespace maicord
