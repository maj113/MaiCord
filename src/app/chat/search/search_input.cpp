#include "search_internal.hpp"
#include "../../shell/overlay_input.hpp"
#include "../../shell/input.hpp"
#include "../../shell/window_events.hpp"
#include "../../shell/click_count.hpp"
#include "maicord/app/shell/shortcut_registry.hpp"
#include "maicord/ui/core/scene_cursor.hpp"
#include "../composer/text_input.hpp"
#include "../composer/text_edit_keys.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {

bool application_search_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto* s = g_app.search;
    if (s && ui_input_consume_key_tail(&s->held_key, {kind, key, repeated}))
        return true;
    if (!front())
        return false;
    const bool raw = application_control_down(), alt = application_alt_down(),
               control = raw && !alt, shift = application_shift_down();
    if (kind == UiInputKind::key_down && shortcut_matches(ShortcutAction::search, shortcut_chord_from_keyboard(key))) {
        if (!s) {
            if (!application_search_synchronize())
                return true;
            s = g_app.search;
        }
        current_scope(*s);
        ApplicationThreadBrowserSearchScope threads{};
        if (application_thread_browser_search_scope(&threads)) {
            // The open thread browser is the focused pane: the shortcut
            // scopes the field to its threads and selects the last query so
            // typing replaces it. Shift widens this session to the server.
            s->widened_thread_session = shift ? threads.session : 0;
            if (shift)
                message_composer_clear(&s->edit);
            else
                message_composer_select_all(&s->edit);
            s->suggestions = shift;
            s->filters = s->sort_menu = false;
            focus(*s, true);
            s->held_key = key;
            repaint();
            return true;
        }
        if (!shift && s->scope == MessageSearchScope::guild) {
            char prefill[32] = {'i', 'n', ':'};
            const auto n = decimal_write(prefill + 3, g_app.account_ui->active_channel_id);
            prefill[n + 3] = ' ';
            message_composer_restore(&s->edit, 0, {prefill, n + 4}, n + 4, n + 4);
        } else if (shift)
            message_composer_clear(&s->edit);
        s->panel = true;
        suggestions_open(*s);
        focus(*s, true);
        s->held_key = key;
        repaint();
        return true;
    }
    auto* rich_scene = s ? ui_search_scene_message_scene(&s->scene) : nullptr;
    const bool rich_focused = rich_scene && g_app.context_focus == &rich_scene->widgets;
    if (!s || (!s->focused && g_app.context_focus != &s->scene.widgets && !rich_focused))
        return false;
    const auto activate_focused = [&]() noexcept {
        const auto h = s->scene.widgets.focused;
        if (rich_focused)
            activate(*s, ui_search_scene_rich_widget_target(s->scene, rich_scene->widgets.focused));
        else if (h.generation && h.slot < s->scene.storage.widgets.count)
            activate(*s, s->scene.targets[h.slot]);
    };
    if (kind == UiInputKind::character) {
        if (!s->focused || control || (alt && !raw) || key < 32 || key == 127 || key == 0x2028 ||
            key == 0x2029)
            return true;
        if (message_composer_insert_utf16(&s->edit, static_cast<u16>(key),
                                          clock_monotonic_milliseconds()) == MessageComposerResult::changed)
            application_search_edit_changed(true);
        return true;
    }
    if (kind == UiInputKind::key_up)
        return true;
    if (kind != UiInputKind::key_down)
        return false;
    if (s->focused && !raw && !alt && !shift && application_composer_text_input_composing() &&
        (key == key::up || key == key::down || key == key::enter || key == key::tab))
        return true;
    if (s->focused && !raw && !alt && !shift && s->suggestions && s->choices &&
        s->choices->filter.kind != UiSearchFilterKind::none) {
        const bool current_choices = choices_current(s);
        if (current_choices) {
            if (key == key::up || key == key::down) {
                choice_move(*s, key == key::up);
                return true;
            }
            if ((key == key::enter || key == key::tab) &&
                s->choices->source.selected < s->choices->source.rows.count) {
                s->held_key = key;
                choice_accept(*s, s->choices->source.revision, s->choices->source.selected);
                return true;
            }
            if (key == key::tab) {
                // An empty/unselected chooser has no completion to accept.
                // Dismiss it before the ordinary editor-to-control focus move.
                s->suggestions = false;
                repaint();
            }
        }
        if (!current_choices && (key == key::up || key == key::down || key == key::enter || key == key::tab))
            return true;
    }
    if (control && key == 'S' && rich_focused) {
        auto target = ui_search_scene_rich_widget_target(s->scene, rich_scene->widgets.focused);
        if (target.action == UiSearchAction::rich_item &&
            target.rich_kind == MessageSceneRichItemKind::attachment) {
            target.save = true;
            activate(*s, target);
        }
        s->held_key = key;
        return true;
    }
    if (key == key::escape) {
        s->held_key = key;
        if (s->filters || s->suggestions || s->sort_menu) {
            menus_close(*s);
            repaint();
        } else
            activate(*s, {UiSearchAction::close, 0, 0});
        return true;
    }
    if (key == key::enter) {
        s->held_key = key;
        if (s->focused)
            commit(*s);
        else
            activate_focused();
        return true;
    }
    if (key == key::space && !s->focused) {
        s->held_key = key;
        activate_focused();
        return true;
    }
    if (key == key::tab) {
        if (shift && s->focused && s->choices && s->suggestions &&
            s->choices->filter.kind != UiSearchFilterKind::none) {
            s->suggestions = false;
            repaint();
        }
        if (rich_focused) {
            const auto before = rich_scene->widgets.focused;
            const auto target = ui_search_scene_rich_widget_target(s->scene, before);
            const auto after = ui_widget_manager_move_focus(&rich_scene->widgets, shift);
            if (after.generation && before.generation &&
                (shift ? after.slot >= before.slot : after.slot <= before.slot)) {
                if (target.source_index < s->scene.measured_count) {
                    ui_search_scene_scroll(
                        &s->scene,
                        maximum(s->scene.scroll_dips, s->scene.row_offsets[target.source_index + 1] -
                                                          ui_rect_height(s->scene.body)));
                    repaint();
                }
                for (u32 i = 0; i < s->scene.widgets.count; ++i)
                    if (s->scene.targets[i].action == UiSearchAction::result &&
                        s->scene.targets[i].source_index == target.source_index) {
                        focus_widget(*s, i);
                        g_app.context_focus = &s->scene.widgets;
                        break;
                    }
            }
            application_request_frame();
            return true;
        }
        if (s->filters || s->suggestions || s->sort_menu) {
            const auto start = s->scene.widgets.focused.slot;
            u32 found = start;
            for (u32 attempt = 0; attempt < s->scene.widgets.count; ++attempt) {
                found = shift ? (found ? found - 1 : s->scene.widgets.count - 1)
                              : (found + 1) % s->scene.widgets.count;
                const auto action = s->scene.targets[found].action;
                const bool filter =
                    ui_search_action_filter(action) || action == UiSearchAction::more_filters ||
                    action == UiSearchAction::clear_recent || action == UiSearchAction::recent;
                const bool sort = action == UiSearchAction::newest || action == UiSearchAction::oldest ||
                                  action == UiSearchAction::relevance;
                if ((s->sort_menu && sort) || (!s->sort_menu && filter)) {
                    focus(*s, false);
                    g_app.context_focus = &s->scene.widgets;
                    focus_widget(*s, found);
                    repaint();
                    break;
                }
            }
            return true;
        }
        focus(*s, false);
        const auto h = ui_widget_manager_move_focus(&s->scene.widgets, shift);
        if (h.generation && s->scene.targets[h.slot].action == UiSearchAction::edit)
            focus(*s, true);
        g_app.context_focus = &s->scene.widgets;
        repaint();
        return true;
    }
    if (!s->focused && (key == key::left || key == key::right)) {
        const auto h = s->scene.widgets.focused;
        if (h.generation && h.slot < s->scene.storage.widgets.count &&
            s->scene.targets[h.slot].action == UiSearchAction::tab) {
            auto target = s->scene.targets[h.slot];
            target.source_index =
                (target.source_index + (key == key::left ? kMessageSearchTabCount - 1 : 1)) %
                kMessageSearchTabCount;
            activate(*s, target);
        }
        return true;
    }
    if (!s->focused)
        return true;
    const u32 revision = s->edit.text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (key == key::backspace) {
        s->held_key = key;
        // Backspace in the empty field removes the scope chip, as with
        // other chip inputs; a held key never reaches past the query.
        if (!repeated && !s->edit.bytes && thread_scope(*s, nullptr))
            return activate(*s, {UiSearchAction::scope, 0, 0});
    }
    if (control && (key == 'C' || key == 'X')) {
        const auto first = minimum(s->edit.anchor, s->edit.caret),
                   last = maximum(s->edit.anchor, s->edit.caret);
        if (last > first && clipboard_write_utf8({s->edit.text + first, last - first}) &&
            key == 'X')
            result = message_composer_replace_range(&s->edit, first, last, {});
    } else if (control && key == 'V') {
        char text[kMessageSearchContentBytes];
        const auto read = clipboard_read_utf8({text, sizeof(text)});
        bool line = read.ok();
        for (u32 i = 0; line && i < read.bytes; ++i)
            line = static_cast<u8>(text[i]) >= 32;
        if (line)
            result = message_composer_insert_utf8(&s->edit, {text, read.bytes});
    } else
        message_composer_edit_key(&s->edit, key, control, shift, &result);
    if (result == MessageComposerResult::changed)
        application_search_edit_changed(revision != s->edit.text_revision);
    return true;
}
bool application_search_window_event(const WindowEvent& event) noexcept {
    auto* s = g_app.search;
    if (!s)
        return false;
    if (input_focus_lost(event)) {
        menus_close(*s);
        focus(*s, false);
        release_pointer(*s);
        return false;
    }
    if (event.type == EventType::capture_lost && event.focus.other != g_app.window) {
        release_pointer(*s);
        return false;
    }
    if (!front() || !(input_pointer(event) || event.type == EventType::cursor_query ||
                      event.type == EventType::context_menu || event.type == EventType::pointer_leave))
        return false;
    const float scale = 96.f / g_app.renderer.dpi;
    float x = 0, y = 0;
    if (event.type == EventType::cursor_query) {
        UiPoint cursor{};
        if (!client_cursor_dip(event, &cursor))
            return false;
        x = cursor.x;
        y = cursor.y;
    } else if (event.type == EventType::wheel) {
        const UiPoint wheel = client_wheel_dip(event);
        x = wheel.x;
        y = wheel.y;
    } else if (event.type == EventType::context_menu) {
        if ((event.flags & ev_keyboard) != 0)
            return g_app.context_focus == &s->scene.widgets;
        const UiPoint context = client_context_dip(event);
        x = context.x;
        y = context.y;
    } else if (input_pointer(event)) {
        x = client_dip_x(event);
        y = client_dip_y(event);
    }
    const bool inside = ui_search_scene_contains(s->scene, x, y);
    const auto hit = ui_search_scene_hit_test(s->scene, x, y);
    // A second press anywhere but the field is another press.
    const bool edit_double = input_double(event) && hit.action == UiSearchAction::edit;
    const bool press = input_press(event) || (input_double(event) && !edit_double);
    if (event.type == EventType::cursor_query && inside) {
        application_set_cursor(ui_search_scene_cursor(s->scene, x, y));
        return true;
    }
    if (event.type == EventType::pointer_leave) {
        if (ui_widget_manager_hover(&s->scene.widgets, ui_widget_invalid_handle()))
            application_request_frame();
        return false;
    }
    if (input_vertical_wheel(event) && inside) {
        if (choices_current(s)) {
            choice_move(*s, event.wheel.dy120 > 0);
            return true;
        }
        if (s->panel && ui_rect_contains(s->scene.body, x, y) && !s->filters && !s->suggestions &&
            !s->sort_menu) {
            const i32 delta = static_cast<i16>(event.wheel.dy120);
            const WheelScroll wheel_scroll = application_wheel_scroll();
            ui_scroll_host_wheel(&g_app.scroll_host,
                                 {s, s->edit.document_identity, scroll_read, scroll_write}, {x, y}, delta,
                                 wheel_scroll.lines, wheel_scroll.page,
                                 application_now_us(), scale);
            application_request_frame();
        }
        return true;
    }
    if (press) {
        if (!inside) {
            const bool popup = s->suggestions || s->filters || s->sort_menu;
            menus_close(*s);
            if (s->focused) {
                focus(*s, false);
                repaint();
            }
            if (popup) {
                repaint();
                return true;
            }
            return false;
        }
        if (hit.action == UiSearchAction::none && (s->suggestions || s->filters || s->sort_menu)) {
            menus_close(*s);
            focus(*s, false);
            reset_value(&s->pressed);
            repaint();
            return true;
        }
        window_focus(g_app.window);
        if (hit.action == UiSearchAction::scope) {
            // The chip acts on press, like the field it sits in: the press
            // itself would blur the field and retire a release target.
            reset_value(&s->pressed);
            activate(*s, hit);
            return true;
        }
        s->pressed = hit;
        s->pressed_revision = s->scene.revision;
        const auto scrollbar = body_scrollbar(*s);
        if (s->panel && !s->filters && !s->suggestions && !s->sort_menu &&
            ui_scrollbar_track_hit(scrollbar, x, y)) {
            focus(*s, false);
            reset_value(&s->pressed);
            s->scrollbar_dragging = ui_scrollbar_thumb_hit(scrollbar, x, y);
            s->thumb_grab = y - scrollbar.thumb.top;
            if (!s->scrollbar_dragging)
                ui_search_scene_scroll(&s->scene,
                                       s->scene.scroll_dips + (y < scrollbar.thumb.top ? -1 : 1) *
                                                                  ui_rect_height(s->scene.body));
            s->captured = true;
            window_capture_pointer(g_app.window);
            repaint();
            return true;
        }
        if (hit.action == UiSearchAction::edit) {
            suggestions_open(*s);
            focus(*s, true);
            // A third press after a double click selects the whole query.
            const bool triple = application_click_count(event, &s->edit, x, y, false) == 3;
            u32 position = 0;
            if (triple)
                message_composer_select_all(&s->edit);
            else if (text_edit_layout_hit_test(g_app.renderer.search_layout, x, y, &position)) {
                u32 byte = 0;
                if (utf8_byte_offset_for_utf16_position(message_composer_text(s->edit), position, &byte))
                    message_composer_set_selection(&s->edit, byte, byte);
            }
            s->dragging = !triple;
            application_search_edit_changed(false);
        } else if (hit.action == UiSearchAction::filter_value) {
            // The chooser is part of the current text edit; preserve its TSF
            // target and selection throughout the pointer capture.
            focus(*s, true);
            if (choices_current(s) && hit.source_revision == s->choices->source.revision &&
                hit.source_index < s->choices->source.rows.count) {
                s->choices->source.selected = static_cast<u16>(hit.source_index);
                ++s->content.revision;
                repaint();
            }
        } else {
            focus(*s, false);
            const auto h = ui_widget_manager_hit_test(s->scene.widgets, x, y);
            if (h.generation) {
                ui_widget_manager_focus(&s->scene.widgets, h);
                ui_widget_manager_capture(&s->scene.widgets, h);
            }
            auto* rich = ui_search_scene_message_scene(&s->scene);
            if (hit.action == UiSearchAction::rich_item && !hit.save && rich) {
                const auto native = ui_widget_manager_hit_test(rich->widgets, x, y);
                if (native.generation) {
                    ui_widget_manager_focus(&rich->widgets, native);
                    g_app.context_focus = &rich->widgets;
                } else
                    g_app.context_focus = &s->scene.widgets;
            } else
                g_app.context_focus = &s->scene.widgets;
            application_request_frame();
        }
        s->pressed_revision = s->scene.revision;
        s->captured = true;
        window_capture_pointer(g_app.window);
        return true;
    }
    if (event.type == EventType::pointer_move && s->scrollbar_dragging) {
        const auto geometry = body_scrollbar(*s);
        ui_search_scene_scroll(&s->scene, ui_scrollbar_scroll_from_pointer(geometry, y, s->thumb_grab));
        repaint();
        return true;
    }
    if (event.type == EventType::pointer_move && s->dragging) {
        u32 position = 0, byte = 0;
        if (text_edit_layout_hit_test(g_app.renderer.search_layout, x, y, &position) &&
            utf8_byte_offset_for_utf16_position(message_composer_text(s->edit), position, &byte)) {
            message_composer_set_selection(&s->edit, s->edit.anchor, byte);
            application_search_edit_changed(false);
        }
        return true;
    }
    if (event.type == EventType::pointer_move) {
        const auto h =
            inside ? ui_widget_manager_hit_test(s->scene.widgets, x, y) : ui_widget_invalid_handle();
        if (ui_widget_manager_hover(&s->scene.widgets, h))
            application_request_frame();
    }
    if (input_release(event) && (s->captured || inside)) {
        const auto pressed = s->pressed;
        const bool drag = s->dragging || s->scrollbar_dragging;
        release_pointer(*s);
        if (!drag && s->pressed_revision == s->scene.revision && ui_search_target_equal(pressed, hit))
            activate(*s, pressed);
        return true;
    }
    if (edit_double) {
        focus(*s, true);
        application_click_count(event, &s->edit, x, y, true);
        message_composer_select_word(&s->edit, s->edit.caret);
        application_search_edit_changed(false);
        return true;
    }
    return inside && (event.type == EventType::pointer_move || event.type == EventType::context_menu ||
                      input_side_button(event) || input_double(event));
}

} // namespace maicord::app_detail
