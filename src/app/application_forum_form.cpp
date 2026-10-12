#include "application_click_count.hpp"
#include "application_forum_form_state.hpp"
#include "application_cursor.hpp"
#include "application_state.hpp"
#include "application_forum_attachments.hpp"
#include "application_forum_preview.hpp"
#include "application_text_form.hpp"
#include "maicord/platform/clipboard_attachment.hpp"
#include "maicord/protocol/writer.hpp"
#include "application_composer_text_input.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_message_interaction.hpp"
#include "application_scrolling.hpp"
#include "application_shell_input.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail {
namespace {
ApplicationForumForm* owner;
u32 held_key;
u64 next_surface_serial;
bool equal(ApplicationForumPostTicket a, ApplicationForumPostTicket b) noexcept {
    return a.serial == b.serial && a.parent_id == b.parent_id &&
           a.publication_revision == b.publication_revision;
}
bool valid(const ApplicationForumForm* f) noexcept {
    return f && f == owner && g_app.renderer_ready && g_app.account_ui && !g_app.login.visible &&
           application_forum_post_current(f->ticket);
}
Utf8View text(const ApplicationForumFormField& field) noexcept {
    const auto v = secure_edit_view(field.edit);
    return {reinterpret_cast<const char*>(v.data), v.count};
}
i32 focused_field(const ApplicationForumForm* f) noexcept {
    const auto focused = forum_post_focused(f->scene).element;
    return focused == ForumPostElement::title ? 0 : focused == ForumPostElement::body ? 1 : -1;
}
void end_capture(ApplicationForumForm* f) noexcept {
    f->dragging = false;
    f->pointer_button = ui_widget_invalid_handle();
    ui_widget_manager_capture(&f->scene.widgets, ui_widget_invalid_handle());
    if (GetCapture() == g_app.window)
        ReleaseCapture();
}
void reset_caret(ApplicationForumForm* f) noexcept {
    f->caret_visible = f->editable && focused_field(f) >= 0 && GetFocus() == g_app.window;
    f->blink_at_ms =
        f->caret_visible && f->blink_ms != INFINITE ? clock_monotonic_milliseconds() + f->blink_ms : 0;
}
// Suffixes are lowercase literals.
bool file_suffix(Utf8View name, Utf8View suffix) noexcept {
    return name.bytes >= suffix.bytes &&
           ascii_equal_ignore_case({name.data + name.bytes - suffix.bytes, suffix.bytes}, suffix);
}
Utf8View file_kind(Utf8View name) noexcept {
    if (file_suffix(name, utf8_literal(".png")) || file_suffix(name, utf8_literal(".jpg")) ||
        file_suffix(name, utf8_literal(".jpeg")) || file_suffix(name, utf8_literal(".webp")) ||
        file_suffix(name, utf8_literal(".gif")))
        return utf8_literal("Image");
    if (file_suffix(name, utf8_literal(".mp4")) || file_suffix(name, utf8_literal(".webm")) ||
        file_suffix(name, utf8_literal(".mov")))
        return utf8_literal("Video");
    if (file_suffix(name, utf8_literal(".pdf")))
        return utf8_literal("PDF");
    return utf8_literal("File");
}
Utf8View file_status(ApplicationForumForm* f, u32 index,
                     const ApplicationForumAttachmentRow& row) noexcept {
    ProtocolWriter writer{
        {reinterpret_cast<u8*>(f->attachment_labels[index]), sizeof(f->attachment_labels[index])}, 0};
    const auto kind = file_kind(row.filename);
    if (!protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(kind.data), kind.bytes) ||
        !protocol_writer_append_literal(&writer, " · ") ||
        !protocol_writer_append_u64(&writer, row.bytes) ||
        !protocol_writer_append_literal(&writer, " bytes · ") ||
        !protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(row.status.data),
                                      row.status.bytes))
        return row.status;
    if (row.phase == MessageAttachmentDraftPhase::uploading && row.bytes) {
        const u32 percent =
            static_cast<u32>(minimum(100., static_cast<double>(row.uploaded_bytes) * 100. / row.bytes));
        if (!protocol_writer_append_literal(&writer, " (") ||
            !protocol_writer_append_u64(&writer, percent) || !protocol_writer_append_literal(&writer, "%)"))
            return row.status;
    }
    return {f->attachment_labels[index], writer.used};
}
bool project(ApplicationForumForm* f) noexcept {
    if (!valid(f) || f->child_suspended)
        return false;
    const auto v = application_forum_post_view();
    if (!equal(v.ticket, f->ticket) || v.tags.count > kForumPostTagCapacity)
        return false;
    const bool editability_changed = f->editable != v.can_edit;
    const bool became_readonly = f->editable && !v.can_edit;
    if (became_readonly) {
        for (auto& field : f->fields) {
            secure_text_store_unbind(&field.store);
            secure_text_store_detach(&field.store);
            secure_edit_cancel_composition(&field.edit);
            field.composition = {};
        }
        end_capture(f);
        ui_button_keyboard_cancel(&f->keyboard);
    }
    if (f->uncertain != v.uncertain)
        f->risk_acknowledged = false;
    f->uncertain = v.uncertain;
    f->editable = v.can_edit;
    if (editability_changed)
        reset_caret(f);
    f->owner_revision = v.revision;
    f->metadata_revision = v.metadata_revision;
    const auto previous_target = forum_post_focused(f->scene);
    const u64 focused_tag = previous_target.element == ForumPostElement::tags &&
                                    previous_target.tag < f->scene.content.tags.count
                                ? f->tags[previous_target.tag].id
                                : 0;
    const auto generation = f->scene.widgets.generation;
    ForumPostContent c{};
    c.style = ui_theme_forum_style(application_theme());
    c.measure_revision = renderer_text_measure_revision(g_app.renderer);
    c.owner_serial = f->surface_serial;
    c.heading = utf8_literal("Create a forum post");
    c.context = v.parent_name;
    c.title_label = utf8_literal("Title");
    c.body_label = utf8_literal("Message");
    c.hint =
        utf8_literal("Ctrl+Enter to post");
    c.tags_hint = v.require_tag  ? utf8_literal("Choose at least one tag (up to 5).")
                  : v.tags.count ? utf8_literal("Tags are optional. Choose up to 5.")
                                 : Utf8View{};
    c.status = f->local_status.bytes ? f->local_status : v.status;
    c.cancel = utf8_literal("Cancel");
    c.accept = v.can_retry ? utf8_literal("Retry") : utf8_literal("Create post");
    c.inspect = v.uncertain ? utf8_literal("View posts") : Utf8View{};
    c.risk = v.uncertain ? utf8_literal("Post anyway") : Utf8View{};
    c.editable = v.can_edit;
    c.can_accept = v.can_submit || v.can_retry;
    c.focus_visible = g_app.keyboard_focus_visible;
    c.uncertain = v.uncertain;
    c.risk_acknowledged = f->risk_acknowledged;
    for (u32 i = 0; i < v.tags.count; ++i)
        f->tags[i] = {v.tags[i].id, forum_post_tag_label(v.tags[i].name), v.tags[i].selected,
                      v.tags[i].enabled || v.tags[i].selected};
    u32 tag_count = v.tags.count;
    for (u32 i = 0; i < v.selected_tags.count; ++i) {
        const u64 id = v.selected_tags[i];
        bool present = false;
        for (u32 j = 0; j < tag_count; ++j)
            if (f->tags[j].id == id) {
                present = true;
                break;
            }
        if (!present) {
            if (tag_count == kForumPostTagCapacity)
                return false;
            f->tags[tag_count++] = {id, utf8_literal("Unavailable tag"), true, true};
        }
    }
    c.tags = {f->tags, tag_count};
    const auto files = application_forum_attachments_view(f->ticket);
    if (files.rows.count > kUiAttachmentDraftMaximum)
        return false;
    f->attachment_revision = files.revision;
    f->preview_revision = application_forum_preview_revision();
    for (u32 i = 0; i < files.rows.count; ++i) {
        const auto& row = files.rows[i];
        const auto phase =
            row.phase == MessageAttachmentDraftPhase::waiting_url ? UiAttachmentDraftPhase::waiting
            : row.phase == MessageAttachmentDraftPhase::uploading ? UiAttachmentDraftPhase::uploading
            : row.phase == MessageAttachmentDraftPhase::failed    ? UiAttachmentDraftPhase::failed
                                                                  : UiAttachmentDraftPhase::ready;
        const auto preview = application_forum_preview_view(f->ticket, row.id);
        f->attachments[i] = {row.id,          row.filename,  file_status(f, i, row),
                             row.description, row.bytes,     row.uploaded_bytes,
                             preview.key,     phase,         row.spoiler,
                             row.can_remove,  row.can_retry, v.can_edit && row.can_remove,
                             preview.ready,   preview.failed};
    }
    c.attachments = {f->attachments, files.rows.count};
    c.add_files = utf8_literal("Add files");
    c.can_add_files = files.can_add;
    ProtocolWriter count{{reinterpret_cast<u8*>(f->attachment_count), sizeof(f->attachment_count)}, 0};
    if (!protocol_writer_append_u64(&count, files.rows.count) ||
        !protocol_writer_append_literal(&count, " / 10 files"))
        return false;
    c.attachment_status = {f->attachment_count, count.used};
    if (!f->local_status.bytes && !v.error && !v.uncertain && files.status.bytes)
        c.status = files.status;
    c.can_accept = c.can_accept && files.ready && !files.busy;

    const i32 focused = focused_field(f);
    for (u32 i = 0; i < 2; ++i) {
        auto& field = f->fields[i];
        SecureEditUtf16Selection selection{};
        if (secure_edit_utf16_selection(field.edit, &selection) != SecureEditResult::unchanged)
            return false;
        secure_text_store_composition(field.store, &field.composition);
        c.fields[i] = {text(field),
                       v.can_edit ? selection.anchor : selection.caret,
                       selection.caret,
                       {field.composition.spans, field.composition.span_count},
                       f->caret_visible && focused == static_cast<i32>(i)};
    }
    if (!forum_post_update(&f->scene, c, application_viewport_dips(),
                           &g_app.account_ui->text_measure_cache))
        return false;
    if (f->scene.widgets.generation != generation) {
        end_capture(f);
        ui_button_keyboard_cancel(&f->keyboard);
        if (previous_target.element == ForumPostElement::attachments) {
            auto restore = previous_target;
            restore.attachment_widget = {};
            if (!forum_post_focus(&f->scene, restore))
                forum_post_focus(&f->scene, {ForumPostElement::add_files, 0});
        }
        if (focused_tag) {
            bool restored = false;
            for (u32 i = 0; i < tag_count; ++i)
                if (f->tags[i].id == focused_tag) {
                    restored = forum_post_focus(&f->scene, {ForumPostElement::tags, i});
                    break;
                }
            if (!restored)
                forum_post_focus(&f->scene, {ForumPostElement::cancel, 0});
        }
    }
    if (became_readonly)
        forum_post_focus(&f->scene, {ForumPostElement::cancel, 0});
    return true;
}
bool refresh(ApplicationForumForm* f) noexcept {
    if (f != owner)
        return false;
    if (f->refreshing)
        return true;
    f->refreshing = true;
    const bool ok = project(f) && renderer_rebuild_content_text(&g_app.renderer, application_theme());
    for (auto& field : f->fields) {
        secure_text_store_synchronize(&field.store);
        secure_text_store_notify_layout(&field.store);
    }
    f->refreshing = false;
    application_request_frame();
    return ok;
}
bool current_layout(ApplicationForumFormField* field) noexcept {
    if (!field || !valid(field->owner) || field->owner->child_suspended || !g_app.window)
        return false;
    const auto& layout = field->owner->surface.layouts[field->index];
    const auto* node = forum_post_node(field->owner->scene,
                                       field->index ? ForumPostElement::body : ForumPostElement::title);
    return layout.active && node && layout.text_signature == text_edit_signature(text(*field)) &&
           layout.viewport_left == node->content_rectangle.left &&
           layout.viewport_top == node->content_rectangle.top &&
           layout.width == ui_rect_width(node->content_rectangle) &&
           layout.height == ui_rect_height(node->content_rectangle);
}
bool geometry(void* context, SecureTextStoreGeometry* output) noexcept {
    auto* field = static_cast<ApplicationForumFormField*>(context);
    if (!output || !current_layout(field))
        return false;
    const auto* node = forum_post_node(field->owner->scene,
                                       field->index ? ForumPostElement::body : ForumPostElement::title);
    const auto bounds = node->content_rectangle;
    const float scale = static_cast<float>(g_app.renderer.dpi) / 96;
    POINT origin{};
    if (!ClientToScreen(g_app.window, &origin) ||
        !GetWindowRect(g_app.window, output->window_screen.native()))
        return false;
    output->text_screen = {origin.x + static_cast<LONG>(bounds.left * scale),
                           origin.y + static_cast<LONG>(bounds.top * scale),
                           origin.x + static_cast<LONG>(bounds.right * scale),
                           origin.y + static_cast<LONG>(bounds.bottom * scale)};
    return true;
}
bool point_geometry(void* context, POINT point, bool round, u32* position, bool* inside) noexcept {
    auto* field = static_cast<ApplicationForumFormField*>(context);
    if (!current_layout(field) || !g_app.renderer.dpi || !ScreenToClient(g_app.window, &point))
        return false;
    const float scale = 96.f / g_app.renderer.dpi;
    return text_edit_layout_point_geometry(field->owner->surface.layouts[field->index], point.x * scale,
                                             point.y * scale, round, position, inside);
}
bool range_geometry(void* context, u32 first, u32 last, RECT* rectangle, bool* clipped) noexcept {
    auto* field = static_cast<ApplicationForumFormField*>(context);
    if (!rectangle || !clipped || !current_layout(field))
        return false;
    TextEditMetric metrics[kTextEditSelectionCapacity]{};
    u32 count = 0;
    if (!text_edit_layout_range_geometry(field->owner->surface.layouts[field->index], first, last,
                                           {metrics, kTextEditSelectionCapacity}, &count) ||
        !count)
        return false;
    UiRect bounds{metrics[0].left, metrics[0].top, metrics[0].left + metrics[0].width,
                  metrics[0].top + metrics[0].height};
    for (u32 i = 1; i < count; ++i) {
        bounds.left = minimum(bounds.left, metrics[i].left);
        bounds.top = minimum(bounds.top, metrics[i].top);
        bounds.right = maximum(bounds.right, metrics[i].left + metrics[i].width);
        bounds.bottom = maximum(bounds.bottom, metrics[i].top + metrics[i].height);
    }
    const auto* node = forum_post_node(field->owner->scene,
                                       field->index ? ForumPostElement::body : ForumPostElement::title);
    const auto clip = node->content_rectangle;
    *clipped = bounds.left < clip.left || bounds.top < clip.top || bounds.right > clip.right ||
               bounds.bottom > clip.bottom;
    bounds = {maximum(bounds.left, clip.left), maximum(bounds.top, clip.top),
              minimum(bounds.right, clip.right), minimum(bounds.bottom, clip.bottom)};
    POINT origin{};
    if (!ClientToScreen(g_app.window, &origin))
        return false;
    const float scale = static_cast<float>(g_app.renderer.dpi) / 96;
    *rectangle = {origin.x + static_cast<LONG>(bounds.left * scale),
                  origin.y + static_cast<LONG>(bounds.top * scale),
                  origin.x + static_cast<LONG>(bounds.right * scale),
                  origin.y + static_cast<LONG>(bounds.bottom * scale)};
    return true;
}
void changed(void* context) noexcept {
    auto* field = static_cast<ApplicationForumFormField*>(context);
    auto* f = field ? field->owner : nullptr;
    if (!valid(f) || f->refreshing || !f->editable)
        return;
    f->local_status = {};
    application_forum_post_draft_update(f->ticket, text(f->fields[0]), text(f->fields[1]));
    if (f == owner) {
        reset_caret(f);
        refresh(f);
    }
}
bool focus(ApplicationForumForm* f, ForumPostTarget target) noexcept {
    const i32 prior = focused_field(f);
    for (auto& field : f->fields)
        secure_edit_break_history_run(&field.edit);
    if (!forum_post_focus(&f->scene, target))
        return false;
    ui_button_keyboard_cancel(&f->keyboard);
    const i32 next = focused_field(f);
    if (prior >= 0 && prior != next) {
        secure_text_store_unbind(&f->fields[prior].store);
        secure_text_store_detach(&f->fields[prior].store);
        f->fields[prior].composition = {};
    }
    if (next >= 0 && f->editable) {
        auto& field = f->fields[next];
        if (!secure_text_store_bind(&field.store, &field.edit, SecureTextInputScope::text) ||
            !secure_text_store_attach(&field.store))
            return false;
        secure_text_store_set_window_focus(&field.store, GetFocus() == g_app.window);
    }
    reset_caret(f);
    return refresh(f);
}
void dispose(bool restore, bool rebuild, bool cancel) noexcept {
    auto* f = owner;
    if (!f)
        return;
    const auto ticket = f->ticket;
    f->closing = true;
    if (application_text_form_forum_child(ticket))
        application_text_form_close(false, false);
    const auto previous = f->previous_focus;
    const bool composer = f->previous_composer_focus, can_restore = restore && valid(f);
    if ((GetKeyState(VK_RETURN) & 0x8000) != 0)
        held_key = VK_RETURN;
    else if ((GetKeyState(VK_SPACE) & 0x8000) != 0)
        held_key = VK_SPACE;
    // Detach every borrowed renderer/TSF/input reference before notifying owner.
    owner = nullptr;
    g_app.context_focus = nullptr;
    renderer_bind_forum_form(&g_app.renderer, nullptr);
    end_capture(f);
    for (auto& field : f->fields)
        secure_text_store_release(&field.store);
    forum_post_surface_release(&f->surface);
    forum_post_release(&f->scene);
    for (auto& field : f->fields)
        secure_edit_release(&field.edit);
    SecureZeroMemory(f, sizeof(*f));
    VirtualFree(f, 0, MEM_RELEASE);
    if (can_restore && previous.manager) {
        auto* manager = const_cast<UiWidgetManager*>(previous.manager);
        if (ui_widget_manager_focus(manager, previous.handle) ||
            ui_widget_manager_input_enabled(*manager, manager->focused)) {
            g_app.context_focus = manager;
            if (composer && manager == &g_app.message_composer_scene.widgets) {
                g_app.message_composer.focused = true;
                ++g_app.message_composer.revision;
                reset_message_composer_caret_timing(true);
            }
        }
    }
    if (rebuild && g_app.renderer_ready && g_app.account_ui)
        refresh_account_text(true);
    if (cancel)
        application_forum_post_cancel(ticket);
    application_request_frame();
}
bool description_valid(TextFormOwnerKey key) noexcept {
    return valid(owner) && !owner->closing && owner->ticket.serial == key.scope &&
           owner->ticket.parent_id == key.collection && owner->editable &&
           application_forum_attachments_current(owner->ticket, key.item);
}
bool description_save(TextFormOwnerKey key, Utf8View value) noexcept {
    return description_valid(key) && application_forum_attachments_describe(owner->ticket, key.item, value);
}
bool description_open(ApplicationForumForm* f, u64 id) noexcept {
    if (!valid(f) || !f->editable)
        return false;
    const auto rows = application_forum_attachments_view(f->ticket).rows;
    for (u32 i = 0; i < rows.count; ++i)
        if (rows[i].id == id && rows[i].can_remove) {
            ApplicationTextFormRequest request{utf8_literal("Attachment description"),
                                               rows[i].filename,
                                               utf8_literal("Description (up to 1024 characters)"),
                                               utf8_literal("Save description"),
                                               rows[i].description,
                                               {f->ticket.serial, f->ticket.parent_id, id},
                                               description_valid,
                                               description_save,
                                               1024};
            request.maximum_bytes = 4096;
            request.forum_parent = f->ticket;
            return application_text_form_open(request);
        }
    return false;
}
bool activate(ApplicationForumForm* f, ForumPostTarget target) noexcept {
    if (target.element == ForumPostElement::cancel) {
        application_forum_form_close();
        return true;
    }
    if (!valid(f)) {
        dispose(false, true, false);
        return true;
    }
    if (target.element == ForumPostElement::add_files) {
        application_forum_attachments_pick(f->ticket);
        if (owner == f)
            refresh(f);
        return true;
    }
    if (target.element == ForumPostElement::attachments && target.attachment_id) {
        const auto handle = ui_attachment_draft_list_handle(f->scene.attachment_list, target.attachment_id,
                                                            target.attachment_action);
        if (!ui_widget_manager_input_enabled(f->scene.widgets, handle) ||
            (target.attachment_widget.generation &&
             (handle.slot != target.attachment_widget.slot ||
              handle.generation != target.attachment_widget.generation)))
            return true;
        if (target.attachment_action == UiAttachmentDraftAction::describe)
            return description_open(f, target.attachment_id);
        if (target.attachment_action == UiAttachmentDraftAction::remove)
            application_forum_attachments_remove(f->ticket, target.attachment_id);
        else if (target.attachment_action == UiAttachmentDraftAction::retry)
            application_forum_attachments_retry(f->ticket, target.attachment_id);
        else if (target.attachment_action == UiAttachmentDraftAction::spoiler) {
            const auto rows = application_forum_attachments_view(f->ticket).rows;
            for (u32 i = 0; i < rows.count; ++i)
                if (rows[i].id == target.attachment_id) {
                    application_forum_attachments_spoiler(f->ticket, rows[i].id, !rows[i].spoiler);
                    break;
                }
        }
        if (owner == f)
            refresh(f);
        return true;
    }
    if (target.element == ForumPostElement::tags && target.tag < f->scene.content.tags.count) {
        f->local_status = {};
        application_forum_post_tag_toggle(f->ticket, f->tags[target.tag].id);
        if (f == owner)
            refresh(f);
        return true;
    }
    if (target.element == ForumPostElement::risk) {
        f->risk_acknowledged = !f->risk_acknowledged;
        refresh(f);
        return true;
    }
    if (target.element == ForumPostElement::inspect) {
        application_forum_post_inspect(f->ticket);
        return true;
    }
    if (target.element != ForumPostElement::accept)
        return false;
    const auto v = application_forum_post_view();
    if ((!v.can_submit && !v.can_retry) || (v.uncertain && !f->risk_acknowledged))
        return true;
    for (auto& field : f->fields) {
        secure_text_store_composition(field.store, &field.composition);
        if (field.composition.active)
            return true;
    }
    f->local_status = {};
    if (v.can_retry)
        application_forum_post_retry(f->ticket, f->risk_acknowledged);
    else if (application_forum_post_draft_update(f->ticket, text(f->fields[0]), text(f->fields[1])))
        application_forum_post_draft_submit(f->ticket);
    if (f == owner) {
        end_capture(f);
        ui_button_keyboard_cancel(&f->keyboard);
        refresh(f);
    }
    return true;
}
bool apply(ApplicationForumForm* f, u32 field_index, SecureEditResult result) noexcept {
    if (result == SecureEditResult::pending_surrogate)
        return true;
    if (result == SecureEditResult::capacity)
        f->local_status = field_index
                              ? utf8_literal("The message is too long. Use at most 2,000 characters.")
                              : utf8_literal("The title is too long. Use at most 100 characters.");
    else if (result == SecureEditResult::invalid_input || result == SecureEditResult::invalid_utf8 ||
             result == SecureEditResult::invalid_utf16)
        f->local_status = field_index ? utf8_literal("This message contains an unsupported character.")
                                      : utf8_literal("The title must be a single line.");
    else if (result == SecureEditResult::changed) {
        f->local_status = {};
        application_forum_post_draft_update(f->ticket, text(f->fields[0]), text(f->fields[1]));
        if (f != owner)
            return true;
    }
    reset_caret(f);
    refresh(f);
    return true;
}
bool pointer(ApplicationForumForm* f, u32 index, float x, float y, bool extend) noexcept {
    if (!f->editable)
        return false;
    auto& edit = f->fields[index].edit;
    u32 position = 0;
    SecureEditUtf16Selection selection{};
    if (!text_edit_layout_hit_test(f->surface.layouts[index], x, y, &position) ||
        secure_edit_utf16_selection(edit, &selection) != SecureEditResult::unchanged)
        return false;
    if (!extend)
        selection.anchor = position;
    selection.caret = position;
    return apply(f, index, secure_edit_set_utf16_selection(&edit, selection));
}
} // namespace
bool application_forum_form_open(const ApplicationForumPostTicket& ticket) noexcept {
    if (owner || g_app.text_form || !g_app.renderer_ready || !g_app.account_ui || g_app.login.visible ||
        !application_forum_post_current(ticket))
        return false;
    auto* f = static_cast<ApplicationForumForm*>(
        VirtualAlloc(nullptr, sizeof(ApplicationForumForm), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!f)
        return false;
    f->ticket = ticket;
    f->surface_serial = ++next_surface_serial;
    if (!f->surface_serial)
        f->surface_serial = ++next_surface_serial;
    f->blink_ms = GetCaretBlinkTime();
    if (!f->blink_ms)
        f->blink_ms = 500;
    f->caret_visible = true;
    const auto v = application_forum_post_view();
    const Utf8View initial[] = {v.title, v.body};
    bool ok = true;
    for (u32 i = 0; i < 2 && ok; ++i) {
        auto& field = f->fields[i];
        field.owner = f;
        field.index = i;
        ok = secure_edit_initialize(
            &field.edit, i ? kApplicationForumBodyBytes : kApplicationForumTitleBytes, false,
            {i ? kApplicationForumBodyScalars : kApplicationForumTitleScalars, i != 0});
        if (ok) {
            const auto assigned = secure_edit_assign(&field.edit, initial[i]);
            ok = (assigned == SecureEditResult::changed || assigned == SecureEditResult::unchanged) &&
                 secure_edit_enable_history(&field.edit, i ? 32768 : 4096);
        }
    }
    if (!ok) {
        for (auto& field : f->fields)
            secure_edit_release(&field.edit);
        VirtualFree(f, 0, MEM_RELEASE);
        return false;
    }
    auto* prior = g_app.context_focus ? g_app.context_focus : &g_app.message_composer_scene.widgets;
    f->previous_focus = {prior, prior->focused};
    f->previous_composer_focus = g_app.message_composer.focused;
    application_composer_text_input_suspend();
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    ui_scroll_host_cancel(&g_app.scroll_host);
    end_message_text_selection();
    end_message_composer_pointer_selection();
    end_scrollbar_drag();
    end_pane_resize();
    stop_local_typing(g_app.message_composer.channel_id);
    g_app.message_composer.focused = false;
    ++g_app.message_composer.revision;
    reset_message_composer_caret_timing(false);
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    owner = f;
    g_app.context_focus = &f->scene.widgets;
    f->surface.scene = &f->scene;
    renderer_bind_forum_form(&g_app.renderer, &f->surface);
    if ((GetKeyState(VK_RETURN) & 0x8000) != 0)
        held_key = VK_RETURN;
    for (auto& field : f->fields)
        if (!secure_text_store_initialize(
                &field.store, {g_app.window, geometry, changed, &field, range_geometry, point_geometry})) {
            dispose(true, true, false);
            return false;
        }
    if (!refresh(f) || !focus(f, {ForumPostElement::title, 0})) {
        dispose(true, true, false);
        return false;
    }
    return true;
}
void application_forum_form_close(const ApplicationForumPostTicket& ticket) noexcept {
    if (owner && equal(owner->ticket, ticket))
        dispose(false, true, false);
}
void application_forum_form_close(bool restore, bool rebuild) noexcept {
    dispose(restore, rebuild, true);
}
void application_forum_form_release() noexcept {
    dispose(false, false, false);
    held_key = 0;
}
bool application_forum_form_suspend_child(const ApplicationForumPostTicket& ticket) noexcept {
    auto* f = owner;
    if (!valid(f) || !equal(f->ticket, ticket) || !f->editable || f->closing || f->child_suspended)
        return false;
    f->child_previous_focus = forum_post_focused(f->scene);
    f->child_suspended = true;
    end_capture(f);
    ui_button_keyboard_cancel(&f->keyboard);
    held_key = 0;
    for (auto& field : f->fields) {
        secure_edit_break_history_run(&field.edit);
        secure_text_store_unbind(&field.store);
        secure_text_store_detach(&field.store);
        secure_edit_cancel_composition(&field.edit);
        field.composition = {};
    }
    f->caret_visible = false;
    f->blink_at_ms = 0;
    for (auto& field : f->scene.content.fields) {
        field.caret_visible = false;
        field.composition = {};
    }
    ui_widget_manager_focus(&f->scene.widgets, ui_widget_invalid_handle());
    ++f->scene.revision;
    g_app.context_focus = nullptr;
    return true;
}
void application_forum_form_resume_child(const ApplicationForumPostTicket& ticket) noexcept {
    auto* f = owner;
    if (!f || !equal(f->ticket, ticket) || !f->child_suspended || f->closing)
        return;
    f->child_suspended = false;
    if (!valid(f)) {
        dispose(false, true, false);
        return;
    }
    g_app.context_focus = &f->scene.widgets;
    auto previous = f->child_previous_focus;
    previous.attachment_widget = {};
    if (refresh(f) && !focus(f, previous))
        focus(f, {ForumPostElement::cancel, 0});
}
bool application_forum_form_active() noexcept {
    return owner != nullptr;
}
ForumPostScene* application_forum_form_scene() noexcept {
    return owner ? &owner->scene : nullptr;
}
ForumPostSurface* application_forum_form_surface() noexcept {
    return owner ? &owner->surface : nullptr;
}
ApplicationForumForm* application_forum_form_state() noexcept {
    return owner;
}
SecureEdit* application_forum_form_edit(ApplicationForumField field) noexcept {
    const u32 index = static_cast<u32>(field);
    return owner && index < 2 ? &owner->fields[index].edit : nullptr;
}
ApplicationForumFormMetrics application_forum_form_metrics() noexcept {
    return owner ? ApplicationForumFormMetrics{sizeof(*owner),
                                               static_cast<u32>((sizeof(*owner) + 4095) & ~4095u),
                                               owner->ticket.serial, owner->scene.revision}
                 : ApplicationForumFormMetrics{};
}
void application_forum_form_theme_changed() noexcept {
    // This form borrows the shared account measure cache. The theme code
    // invalidates that cache even while this parent is hidden.
    if (owner)
        owner->scene.layout_signature = 0;
}
bool application_forum_form_synchronize() noexcept {
    auto* f = owner;
    if (!f)
        return true;
    if (!valid(f)) {
        dispose(false, true, false);
        return true;
    }
    if (f->child_suspended || application_text_form_forum_child(f->ticket))
        return true;
    const auto v = application_forum_post_view();
    const auto viewport = application_viewport_dips();
    const auto style = ui_theme_forum_style(application_theme());
    if (!f->scene.layout_signature || f->preview_revision != application_forum_preview_revision() || f->owner_revision != v.revision ||
        f->metadata_revision != v.metadata_revision ||
        f->attachment_revision != application_forum_attachments_view(f->ticket).revision ||
        f->scene.content.focus_visible != g_app.keyboard_focus_visible ||
        f->scene.content.measure_revision != renderer_text_measure_revision(g_app.renderer) ||
        !bytes_equal(&f->scene.content.style, &style, sizeof(style)) ||
        !ui_rect_equal(viewport, f->scene.viewport)) {
        const bool was = f->editable;
        const bool ok = refresh(f);
        if (ok && !was && f->editable && focused_field(f) >= 0)
            focus(f, forum_post_focused(f->scene));
        return ok;
    }
    return true;
}
bool application_forum_form_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if (ui_input_consume_key_tail(&held_key, {kind, code, repeated}))
        return true;
    auto* f = owner;
    if (!f || f->child_suspended || application_text_form_forum_child(f->ticket))
        return false;
    if (!application_forum_form_synchronize() || owner != f)
        return true;
    if (kind == UiInputKind::system_key_down)
        return code != VK_F4 && code != VK_SPACE;
    if (kind == UiInputKind::system_character)
        return code != ' ';
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0,
               shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (kind == UiInputKind::key_down && code == VK_ESCAPE) {
        held_key = code;
        application_forum_form_close();
        return true;
    }
    if (kind == UiInputKind::key_down && code == VK_TAB) {
        const auto prior = forum_post_focused(f->scene);
        if (forum_post_move_focus(&f->scene, shift)) {
            const auto next = forum_post_focused(f->scene);
            // Focus movement happened in the retained manager; clear both native bindings.
            for (auto& field : f->fields) {
                secure_text_store_unbind(&field.store);
                secure_text_store_detach(&field.store);
                field.composition = {};
            }
            (void)prior;
            focus(f, next);
        }
        return true;
    }
    if (kind == UiInputKind::key_down && ctrl && code == VK_RETURN) {
        if (!repeated) {
            held_key = code;
            activate(f, {ForumPostElement::accept, 0});
        }
        return true;
    }
    if (kind == UiInputKind::key_down && ctrl && code == 'V' && f->editable &&
        clipboard_attachment_available()) {
        if (!repeated) {
            application_forum_attachments_paste(f->ticket);
            if (owner == f)
                refresh(f);
        }
        return true;
    }
    const i32 focused = focused_field(f);
    if (focused < 0 || !f->editable) {
        if (kind == UiInputKind::key_down || kind == UiInputKind::key_up) {
            const auto target = forum_post_focused(f->scene);
            const auto key = code == VK_RETURN  ? UiButtonKey::enter
                             : code == VK_SPACE ? UiButtonKey::space
                                                : UiButtonKey::other;
            const auto action = ui_button_keyboard_key(&f->keyboard, &f->scene.widgets, key,
                                                       kind == UiInputKind::key_down, repeated);
            if (action.activate) {
                activate(f, target);
                if (owner != f && kind == UiInputKind::key_down)
                    held_key = code;
            }
        }
        return true;
    }
    const u32 index = static_cast<u32>(focused);
    auto& field = f->fields[index];
    auto& edit = field.edit;
    secure_text_store_composition(field.store, &field.composition);
    if (kind == UiInputKind::character) {
        if (ctrl || code == '\b' || code == '\t' || code == 27 ||
            (!index && (code == '\r' || code == '\n')))
            return true;
        return code <= 0xFFFF
                   ? apply(f, index,
                           secure_edit_insert_utf16(&edit, static_cast<u16>(code == '\r' ? '\n' : code),
                                                    clock_monotonic_milliseconds()))
                   : true;
    }
    if (kind != UiInputKind::key_down)
        return true;
    if (ctrl && (code == 'Z' || code == 'Y'))
        return field.composition.active
                   ? true
                   : apply(f, index,
                           code == 'Y' || shift ? secure_edit_redo(&edit) : secure_edit_undo(&edit));
    if (ctrl && code == 'A')
        return apply(f, index, secure_edit_select_all(&edit));
    if (ctrl && (code == 'C' || code == 'X')) {
        secure_edit_break_history_run(&edit);
        const auto first = minimum(edit.anchor, edit.caret), last = maximum(edit.anchor, edit.caret);
        if (first != last &&
            clipboard_write_utf8(
                g_app.window, {reinterpret_cast<const char*>(edit.storage.data + first), last - first}) &&
            code == 'X')
            return apply(f, index, secure_edit_delete_forward(&edit));
        return true;
    }
    if (ctrl && code == 'V') {
        char pasted[kApplicationForumBodyBytes]{};
        const auto read = clipboard_read_utf8(g_app.window, {pasted, sizeof(pasted)});
        const auto result = read.ok() ? secure_edit_insert_utf8(&edit, {pasted, read.bytes})
                            : read.error == ClipboardTextError::capacity ? SecureEditResult::capacity
                                                                         : SecureEditResult::unchanged;
        SecureZeroMemory(pasted, sizeof(pasted));
        return apply(f, index, result);
    }
    switch (code) {
    case VK_BACK:
        return apply(f, index, secure_edit_backspace(&edit, clock_monotonic_milliseconds()));
    case VK_DELETE:
        return apply(f, index, secure_edit_delete_forward(&edit, clock_monotonic_milliseconds()));
    case VK_LEFT:
        return apply(f, index, secure_edit_move(&edit, SecureEditMove::previous, shift));
    case VK_RIGHT:
        return apply(f, index, secure_edit_move(&edit, SecureEditMove::next, shift));
    case VK_HOME:
        return apply(
            f, index,
            secure_edit_move(&edit, ctrl ? SecureEditMove::home : SecureEditMove::line_home, shift));
    case VK_END:
        return apply(f, index,
                     secure_edit_move(&edit, ctrl ? SecureEditMove::end : SecureEditMove::line_end, shift));
    case VK_UP:
    case VK_DOWN: {
        u32 position = 0;
        SecureEditUtf16Selection selection{};
        if (text_edit_layout_move_vertical(f->surface.layouts[index], code == VK_DOWN, &position) &&
            secure_edit_utf16_selection(edit, &selection) == SecureEditResult::unchanged) {
            if (!shift)
                selection.anchor = position;
            selection.caret = position;
            return apply(f, index, secure_edit_set_utf16_selection(&edit, selection));
        }
        return true;
    }
    default:
        return true;
    }
}
bool application_forum_form_filter_message(const MSG& message) noexcept {
    if (message.hwnd != g_app.window)
        return false;
    if (held_key && (message.message == WM_KEYDOWN || message.message == WM_KEYUP) &&
        message.wParam == held_key)
        return application_forum_form_input(
            message.message == WM_KEYDOWN ? UiInputKind::key_down : UiInputKind::key_up,
            static_cast<u32>(message.wParam), (message.lParam & (1ll << 30)) != 0);
    auto* f = owner;
    if (!f || f->child_suspended || application_text_form_forum_child(f->ticket) ||
        !application_forum_form_synchronize() || owner != f)
        return false;
    // Attachment clipboard formats belong to the post owner before TSF has
    // an opportunity to consume Ctrl+V for the focused text field.
    if (f->editable && message.message == WM_KEYDOWN && message.wParam == 'V' &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0 && clipboard_attachment_available())
        return application_forum_form_input(UiInputKind::key_down, 'V',
                                            (message.lParam & (1ll << 30)) != 0);
    const i32 focused = focused_field(f);
    return focused >= 0 && f->editable &&
           secure_text_store_filter_message(&f->fields[focused].store, message);
}
bool application_forum_form_window_message(UINT message, WPARAM wparam, LPARAM lparam,
                                           LRESULT* result) noexcept {
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE)
        held_key = 0;
    auto* f = owner;
    if (!f || !result || f->child_suspended || application_text_form_forum_child(f->ticket))
        return false;
    if (message == WM_DROPFILES) {
        application_forum_attachments_drop(f->ticket, reinterpret_cast<HANDLE>(wparam));
        if (owner == f && valid(f))
            refresh(f);
        *result = 0;
        return true;
    }
    if (!application_forum_form_synchronize() || owner != f)
        return false;
    *result = 0;
    const float scale = 96.f / static_cast<float>(g_app.renderer.dpi);
    const float x = static_cast<float>(static_cast<short>(LOWORD(lparam))) * scale,
                y = static_cast<float>(static_cast<short>(HIWORD(lparam))) * scale;
    if (message == WM_KILLFOCUS) {
        end_capture(f);
        ui_button_keyboard_cancel(&f->keyboard);
        for (auto& field : f->fields) {
            secure_edit_break_history_run(&field.edit);
            secure_text_store_set_window_focus(&field.store, false);
        }
        reset_caret(f);
        refresh(f);
        return false;
    }
    if (message == WM_SETFOCUS) {
        const i32 focused = focused_field(f);
        if (focused >= 0)
            secure_text_store_set_window_focus(&f->fields[focused].store, true);
        reset_caret(f);
        refresh(f);
        return false;
    }
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        f->dragging = false;
        f->pointer_button = ui_widget_invalid_handle();
        ui_widget_manager_capture(&f->scene.widgets, ui_widget_invalid_handle());
        ui_button_keyboard_cancel(&f->keyboard);
        return false;
    }
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const auto* panel = forum_post_node(f->scene, ForumPostElement::panel);
        if (panel && !ui_rect_contains(panel->rectangle, x, y)) {
            application_forum_form_close();
            return true;
        }
        SetFocus(g_app.window);
        const auto target = forum_post_hit_test(f->scene, x, y);
        if (target.element == ForumPostElement::count)
            return true;
        if (focus(f, target)) {
            const i32 index = focused_field(f);
            UiWidgetHandle handle{};
            if (index >= 0) {
                // Double click: word; third press: line. A composition keeps its selection.
                auto& field = f->fields[index];
                const u8 clicks = application_click_count(&field, x, y, message == WM_LBUTTONDBLCLK);
                SecureTextComposition composition{};
                const bool composing = secure_text_store_composition(field.store, &composition) && composition.active;
                if (!composing) {
                    pointer(f, static_cast<u32>(index), x, y, clicks == 1 && (wparam & MK_SHIFT) != 0);
                    if (clicks == 2)
                        apply(f, static_cast<u32>(index), secure_edit_select_word(&field.edit, field.edit.caret));
                    else if (clicks == 3)
                        apply(f, static_cast<u32>(index), secure_edit_select_line(&field.edit, field.edit.caret));
                }
                f->dragging = clicks == 1 && !composing;
                handle = f->scene.elements[static_cast<u32>(target.element)];
            } else {
                handle = target.element == ForumPostElement::attachments ? target.attachment_widget
                         : target.element == ForumPostElement::tags
                             ? f->scene.tags[target.tag]
                             : f->scene.elements[static_cast<u32>(target.element)];
                f->pointer_button = handle;
            }
            if (ui_widget_manager_capture(&f->scene.widgets, handle))
                SetCapture(g_app.window);
            if (GetCapture() != g_app.window)
                end_capture(f);
        }
        return true;
    }
    case WM_MOUSEMOVE:
        if (f->dragging && (wparam & MK_LBUTTON)) {
            const i32 index = focused_field(f);
            if (index >= 0)
                pointer(f, static_cast<u32>(index), x, y, true);
        }
        return true;
    case WM_LBUTTONUP: {
        const auto button = f->pointer_button;
        const auto target = forum_post_hit_test(f->scene, x, y);
        const auto handle =
            target.element == ForumPostElement::attachments ? target.attachment_widget
            : target.element == ForumPostElement::tags && target.tag < f->scene.content.tags.count
                ? f->scene.tags[target.tag]
            : static_cast<u32>(target.element) < kForumPostElements
                ? f->scene.elements[static_cast<u32>(target.element)]
                : ui_widget_invalid_handle();
        const bool clicked =
            button.generation && button.slot == handle.slot && button.generation == handle.generation;
        end_capture(f);
        if (clicked)
            activate(f, target);
        return true;
    }
    case WM_MOUSEWHEEL: {
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ScreenToClient(g_app.window, &point);
        const float delta = -static_cast<float>(static_cast<short>(HIWORD(wparam))) / WHEEL_DELTA * 48;
        const auto target = forum_post_hit_test(f->scene, point.x * scale, point.y * scale);
        if (target.element == ForumPostElement::body)
            text_edit_layout_scroll(&f->surface.layouts[1], delta);
        else
            forum_post_scroll(&f->scene, delta);
        refresh(f);
        return true;
    }
    case WM_MOUSEHWHEEL:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        return true;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            // Title/body editors, tags, attachment and action buttons declare themselves.
            UiPoint point{};
            application_set_cursor(application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point)
                                       ? ui_widget_manager_cursor(f->scene.widgets, point.x, point.y)
                                       : UiCursor::arrow);
            *result = TRUE;
            return true;
        }
        return false;
    default:
        return false;
    }
}
void application_forum_form_tick(u64 now_ms) noexcept {
    auto* f = owner;
    if (!f || f->child_suspended || application_text_form_forum_child(f->ticket))
        return;
    if (!valid(f)) {
        dispose(false, true, false);
        return;
    }
    if (f->blink_at_ms && now_ms >= f->blink_at_ms) {
        f->caret_visible = !f->caret_visible;
        f->blink_at_ms = now_ms + f->blink_ms;
        // Updating a blink must keep its existing deadline and visibility.
        const bool visible = f->caret_visible;
        const u64 next = f->blink_at_ms;
        refresh(f);
        if (owner == f) {
            f->caret_visible = visible;
            f->blink_at_ms = next;
        }
    }
}
u64 application_forum_form_next_wake() noexcept {
    return owner && !owner->child_suspended ? owner->blink_at_ms : 0;
}
} // namespace maicord::app_detail
