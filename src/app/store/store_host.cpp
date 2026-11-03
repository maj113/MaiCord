#include "store_host.hpp"
#include "store_preview.hpp"
#include "store_source.hpp"
#include "../account/account_presentation.hpp"
#include "../chat/application_link_trust.hpp"
#include "../chat/composer/text_edit_keys.hpp"
#include "../effects/effects.hpp"
#include "../plugins/dev_console.hpp"
#include "../packages/packages.hpp"
#include "../plugins/catalog_service.hpp"
#include "../plugins/settings_dialog.hpp"
#include "../settings/custom_theme.hpp"
#include "../settings/catalog.hpp"
#include "../settings/settings.hpp"
#include "../shell/input.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/toast.hpp"
#include "maicord/app/plugins/binding.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/shell.hpp"
#include "maicord/platform/desktop/file_picker.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/widgets/kit_markdown.hpp"

namespace maicord::app_detail {
namespace {

constexpr u64 kMenuOwner = 0x57A0'0001u;
constexpr u64 kTrialIdleMs = 30'000;

struct StoreHost {
    UiStoreScene scene;
    StoreModel model;
    StorePageState pages[3];
    CatalogKind kind;
    bool shown, dirty;
    u64 stamp;
    // Text editors: the page's search and the install dialog's link field.
    MessageComposer query_edit;
    char query_text[kStoreQueryBytes];
    MessageComposer link_edit;
    char link_text[kPackageUrlMaximumBytes + 1];
    UiEditView view;
    UiStoreSearch search;
    UiStoreMeasure measure;
    bool editing;
    u32 editing_action;
    // A menu on the pop-up scene.
    bool menu_open;
    StoreMenu menu;
    u32 menu_item, menu_pressed;
    PopupItem menu_items[16];
    char menu_labels[48];
    // Pointer: the release belongs to the store only when the store took the press.
    bool pressing;
    u32 slider_action;
    bool dragging_scrollbar;
    float scrollbar_grab;
    // Details of the open item.
    UiMarkdownBank* bank;
    UiMarkdown* about;
    UiMarkdown* changes;
    char details[8192];
    char changelog[1024];
    u32 details_bytes, changelog_bytes;
    bool details_loading;
    u8 details_sha[32];
    char details_slug[kCatalogSlugMaximumBytes + 1];
    // A theme being tried.
    bool trying;
    char trying_name[64];
    u32 trying_bytes;
    u64 last_input_ms;
    // Files.
    NativeFilePicker picker;
    bool picking;
    u8 pick_for; // 0 link dialog, 1 dev load
    u32 revisions[2];
    bool swallow_char;
    bool trial_asked; // the open card's effect trial was started
    u32 preview_changes;
    UiRect host_page, host_clip;
    // The pictures of the visible tiles and the loop of the card under the pointer or focus.
    StorePreviews previews;
    u32 hover_slot;
    u64 hover_since_ms;
};

StoreHost* g = nullptr;

StorePageState& page() noexcept {
    return g->pages[static_cast<u32>(g->kind)];
}

bool ensure() noexcept {
    if (g)
        return true;
    g = static_cast<StoreHost*>(commit_pages(sizeof(StoreHost)));
    if (!g)
        return false;
    for (u32 i = 0; i < 3; ++i)
        store_page_reset(&g->pages[i], static_cast<CatalogKind>(i));
    message_composer_initialize(&g->query_edit, {g->query_text, kStoreQueryBytes});
    message_composer_initialize(&g->link_edit, {g->link_text, kPackageUrlMaximumBytes});
    g->measure = ui_store_cache_measure(g_app.renderer.message_measure_cache);
    g->scene.measure = &g->measure;
    return ui_store_scene_open(&g->scene);
}

void request() noexcept {
    g->dirty = true;
    application_request_frame();
}

MessageComposer& editor() noexcept {
    return g->editing_action == kStoreLinkField ? g->link_edit : g->query_edit;
}

// The page's copy of the query.
void copy_query(const Utf8View& text) noexcept {
    auto& st = page();
    st.query_bytes = minimum<u32>(text.bytes, kStoreQueryBytes);
    copy_bytes(st.query, text.data, st.query_bytes);
}

// The page's copy of the query follows the editor.
void query_changed() noexcept {
    if (g->editing_action != kStoreLinkField)
        copy_query(message_composer_text(g->query_edit));
    request();
}

void set_query(const Utf8View& text) noexcept {
    message_composer_clear(&g->query_edit);
    if (text.bytes)
        message_composer_insert_utf8(&g->query_edit, text);
    copy_query(text);
}

// The open detail's item, or ~0u.
u32 open_item() noexcept {
    const StorePageState& st = page();
    return store_find_item(g->model, st.kind, {st.slug, c_string_length(st.slug)});
}

void focus_editor(u32 action) noexcept {
    g->editing = true;
    g->editing_action = action;
    editor().focused = true;
    application_focus_window();
    request();
}

void leave_editor() noexcept {
    if (!g->editing)
        return;
    g->editing = false;
    editor().focused = false;
    request();
}

__declspec(noinline) void toast(bool ok, const char* good, const char* bad) noexcept {
    application_toast_result(ok, {good, c_string_length(good)}, {bad, c_string_length(bad)});
}

// ---- Menus -----------------------------------------------------------------------------------------------------
void close_menu() noexcept {
    if (!g || !g->menu_open)
        return;
    g->menu_open = false;
    g->menu_pressed = 0;
    if (g_app.popup_owner_kind == PopupOwnerKind::settings_choice && g_app.popup_scene.owner_id == kMenuOwner) {
        const u32 count = minimum(g_app.popup_scene.item_count, g_app.popup_scene.items.count);
        popup_scene_close(&g_app.popup_scene);
        for (u32 i = 0; i < count; ++i)
            reset_value(&g_app.popup_scene.items[i]);
        g_app.popup_owner_kind = PopupOwnerKind::none;
        g_app.renderer.retained_presentation.valid = false;
    }
    g->dirty = true;
}

// Settings closes the shared choice popup on a capture change or focus loss, the store's menu among them.
bool menu_shown() noexcept {
    if (g->menu_open && !(g_app.popup_scene.active && g_app.popup_scene.owner_id == kMenuOwner))
        close_menu();
    return g->menu_open;
}

bool open_menu(StoreMenu which, u32 item, const UiRect& anchor) noexcept {
    close_menu();
    const u32 count = store_menu_items(g->model, page(), which, item, g->menu_items, 16, {g->menu_labels, sizeof(g->menu_labels)});
    if (!count)
        return false;
    // A menu of a small button hangs from its right edge, the way an overflow menu does.
    UiRect at = anchor;
    if (ui_rect_width(at) < 240.f)
        at.left = at.right - 240.f;
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::settings_choice, kMenuOwner, {g->menu_items, count}, at,
                          application_viewport_dips(), maximum(240.f, ui_rect_width(at)), false)
             .ok())
        return false;
    g_app.popup_owner_kind = PopupOwnerKind::settings_choice;
    g->menu_open = true;
    g->menu = which;
    g->menu_item = item;
    g->menu_pressed = 0;
    u32 selected = 0;
    for (u32 i = 0; i < count; ++i)
        if (g->menu_items[i].flags & popup_item_checked)
            selected = i;
    bool changed = false;
    popup_scene_select_index(&g_app.popup_scene, selected, &changed);
    application_request_frame();
    return true;
}

// ---- Commands --------------------------------------------------------------------------------------------------
void end_trial() noexcept {
    g->trying = false;
    g->trying_bytes = 0;
}

void start_pick(u8 purpose) noexcept {
    if (g->picking)
        return;
    if (native_file_picker_start(&g->picker, g_app.window, 1,
                                 purpose == 0 ? utf8_literal("Choose a theme or effect")
                                              : page().kind == CatalogKind::plugin ? utf8_literal("Load a plugin DLL") : utf8_literal("Load an effect"))
        == NativeFilePickerError::none) {
        g->picking = true;
        g->pick_for = purpose;
    }
}

// The page's dev folder in Explorer.
void open_folder_command() noexcept {
    const Utf8View folder = application_catalog_dev_folder(page().kind == CatalogKind::plugin ? 0 : 1);
    wchar_t path[MAX_PATH];
    const int n = folder.bytes ? MultiByteToWideChar(CP_UTF8, 0, folder.data, static_cast<int>(folder.bytes), path, MAX_PATH - 1) : 0;
    if (n > 0) {
        path[n] = 0;
        ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, 1);
    }
}

void execute(const StoreCommand& c) noexcept {
    StoreModel& m = g->model;
    const StoreItem* item = c.item < m.count ? &m.items[c.item] : nullptr;
    const CatalogKind kind = page().kind;
    switch (c.kind) {
    case StoreCommandKind::menu:
        open_menu(c.menu, c.item, c.anchor);
        break;
    case StoreCommandKind::install:
        if (item)
            toast(application_catalog_install(item->kind, item->slug), {}, "Couldn't start the install. Try again.");
        break;
    case StoreCommandKind::update:
    case StoreCommandKind::review:
        if (item)
            toast(application_catalog_update(item->kind, item->slug), {}, "Couldn't start the update. Try again.");
        break;
    case StoreCommandKind::remove:
        if (item) {
            // A local theme is only its file; catalog and link installs go through the catalog.
            if (item->local)
                toast(application_custom_theme_remove_installed(item->file), {}, "Couldn't remove it. Try again.");
            else
                toast(application_catalog_remove(item->kind, item->slug, c.flag), {}, "Couldn't remove it. Try again.");
            if (item->kind == CatalogKind::theme)
                application_custom_theme_rescan();
            if (page().detail && item->kind != CatalogKind::plugin)
                page().detail = page().knobs = false;
        }
        break;
    case StoreCommandKind::rollback:
        if (item)
            toast(application_catalog_rollback(item->kind, item->slug), {}, "Couldn't roll back. Try again.");
        break;
    case StoreCommandKind::enable:
    case StoreCommandKind::disable:
        if (item)
            toast(application_catalog_set_enabled(item->kind, item->slug, c.kind == StoreCommandKind::enable), {}, "Couldn't change that plugin. Try again.");
        break;
    case StoreCommandKind::set_auto_update:
        if (item)
            application_catalog_set_auto_update(item->kind, item->slug, c.flag);
        break;
    case StoreCommandKind::skip_update:
        if (item)
            application_catalog_skip_update(item->kind, item->slug);
        application_catalog_consent_answer(false, false);
        break;
    case StoreCommandKind::update_all:
        application_catalog_update_all();
        break;
    case StoreCommandKind::check_now:
        application_catalog_check_now();
        break;
    case StoreCommandKind::set_auto_update_all:
        application_catalog_set_auto_update_all(c.flag);
        break;
    case StoreCommandKind::open_plugin_settings:
        if (item && item->kind == CatalogKind::plugin)
            if (PluginRuntime* runtime = application_catalog_runtime())
                for (u32 i = 0; i < runtime->binding_count; ++i)
                    if (runtime->bindings[i] && runtime->bindings[i]->image.descriptor &&
                        runtime->bindings[i]->image.info.name_bytes == item->name.bytes &&
                        bytes_equal(runtime->bindings[i]->image.info.name, item->name.data, item->name.bytes))
                        application_plugin_settings_open(runtime->bindings[i]);
        break;
    case StoreCommandKind::open_folder:
        open_folder_command();
        break;
    case StoreCommandKind::open_source: {
        const u32 at = open_item();
        if (at < m.count && m.items[at].source.bytes)
            application_link_open(m.items[at].source, true);
        break;
    }
    case StoreCommandKind::open_homepage: {
        u32 at = c.arg == 2 ? c.item : open_item();
        if (at < m.count) {
            const StoreItem& open = m.items[at];
            if (c.arg == 1 && open.license.bytes) {
                char url[96];
                static constexpr char kBase[] = "https://spdx.org/licenses/";
                copy_bytes(url, kBase, sizeof(kBase) - 1);
                const u32 n = minimum<u32>(open.license.bytes, 40);
                copy_bytes(url + sizeof(kBase) - 1, open.license.data, n);
                application_link_open({url, static_cast<u32>(sizeof(kBase) - 1 + n)}, true);
            } else if (open.homepage.bytes)
                application_link_open(open.homepage, true);
        }
        break;
    }
    case StoreCommandKind::copy_text: {
        const u32 at = open_item();
        Utf8View text = c.arg == 1 && at < m.count ? m.items[at].sha : m.error_text;
        application_copy_text(text, utf8_literal("Copied"), kToastCopyFailed);
        break;
    }
    case StoreCommandKind::apply_theme:
    case StoreCommandKind::try_theme: {
        // Apply applies as soon as the file has loaded; Try it previews it until Keep or Revert.
        const bool trial = c.kind == StoreCommandKind::try_theme;
        if (!item || item->file == ~0u || !application_custom_theme_apply_installed(item->file, trial)) {
            toast(false, {}, "Couldn't open that theme. Try again.");
            break;
        }
        if (trial) {
            g->trying = true;
            g->trying_bytes = minimum<u32>(item->name.bytes, sizeof(g->trying_name));
            copy_bytes(g->trying_name, item->name.data, g->trying_bytes);
            g->last_input_ms = clock_monotonic_milliseconds();
        }
        break;
    }
    case StoreCommandKind::keep_theme:
        application_custom_theme_action(ApplicationCustomThemeAction::apply);
        end_trial();
        break;
    case StoreCommandKind::revert_theme:
        application_custom_theme_action(ApplicationCustomThemeAction::cancel);
        end_trial();
        break;
    case StoreCommandKind::import_theme:
        application_custom_theme_action(c.arg == 1   ? ApplicationCustomThemeAction::retry_read
                                        : c.arg == 2 ? ApplicationCustomThemeAction::retry_save
                                                     : ApplicationCustomThemeAction::import_file);
        break;
    case StoreCommandKind::use_builtin_theme:
        toast(application_custom_theme_use_builtin(), {}, "Couldn't switch back to the built-in theme. Try again.");
        break;
    case StoreCommandKind::use_effect:
        if (item && item->library != ~0u)
            application_effects_select(item->library);
        break;
    case StoreCommandKind::knob_set:
        if (c.item < m.knob_count) {
            McfxKnobValue value{};
            copy_bytes(&value.number, &c.value, 4);
            application_effects_knob_set(c.item, value, c.flag);
        }
        break;
    case StoreCommandKind::knob_color:
        if (c.item < m.knob_count) {
            McfxKnobValue value{};
            value.rgb = c.value;
            application_effects_knob_set(c.item, value, true);
        }
        break;
    case StoreCommandKind::knobs_reset:
        application_effects_knobs_reset();
        break;
    case StoreCommandKind::universal_set:
    case StoreCommandKind::universal_reset: {
        const bool reset = c.kind == StoreCommandKind::universal_reset;
        const u32 row = c.arg;
        const u32 at = open_item();
        const bool shader = c.flag || reset;
        const u64 identity = at < m.count && m.items[at].library != ~0u ? application_effects_identity(m.items[at].library) : 0;
        EffectUserSettings user = application_effects_user_settings();
        EffectShaderSettings sh = identity ? application_effects_shader_settings(identity) : EffectShaderSettings{};
        const u32 value = reset ? 0 : c.value;
        bool touched_user = false;
        switch (static_cast<StoreUniversalRow>(row)) {
        case StoreUniversalRow::resolution:
            if (shader && identity) {
                sh.resolution = reset || sh.resolution == value + 1 ? 0 : static_cast<u8>(value + 1);
            } else {
                user.resolution = user.resolution == value + 1 ? 0 : static_cast<u8>(value + 1);
                touched_user = true;
            }
            break;
        case StoreUniversalRow::frame_rate:
            if (shader && identity) {
                sh.frame_rate = reset || sh.frame_rate == value + 1 ? 0 : static_cast<u8>(value + 1);
            } else {
                user.frame_rate = user.frame_rate == value + 1 ? 0 : static_cast<u8>(value + 1);
                touched_user = true;
            }
            break;
        case StoreUniversalRow::speed:
            sh.speed = static_cast<u8>(minimum<u32>(value, 60));
            break;
        case StoreUniversalRow::opacity:
            sh.opacity = static_cast<u8>(minimum<u32>(value, 100));
            break;
        case StoreUniversalRow::blend:
            sh.blend = static_cast<u8>(value + 1);
            break;
        case StoreUniversalRow::unfocused:
            user.unfocused = static_cast<EffectUnfocused>(minimum<u32>(value, 2));
            touched_user = true;
            break;
        case StoreUniversalRow::budget:
            user.budget = static_cast<EffectBudget>(minimum<u32>(value, 2));
            touched_user = true;
            break;
        case StoreUniversalRow::pointer:
            sh.pointer = static_cast<u8>(value ? 1 : 0);
            break;
        default:
            break;
        }
        if (touched_user)
            application_effects_set_user_settings(user);
        else if (identity)
            application_effects_set_shader_settings(sh);
        break;
    }
    case StoreCommandKind::consent_answer:
        application_catalog_consent_answer(c.arg == 1, c.flag);
        break;
    case StoreCommandKind::link_begin: {
        const Utf8View text = message_composer_text(g->link_edit);
        PackageLink link{};
        if (package_link_parse(text, &link) && link.type == PackageLinkType::catalog) {
            page().link_dialog = false;
            StorePageState& target = g->pages[static_cast<u32>(link.kind)];
            target.detail = true;
            target.detail_from = StoreTab::browse;
            copy_bytes(target.slug, link.slug, link.slug_bytes);
            target.slug[link.slug_bytes] = 0;
            g->kind = static_cast<CatalogKind>(link.kind);
        } else if (application_packages_begin_link(text)) {
            page().link_dialog = false;
        }
        leave_editor();
        break;
    }
    case StoreCommandKind::link_open_file:
        start_pick(0);
        break;
    case StoreCommandKind::link_commit:
        toast(application_packages_commit() == PackageError::none, "Installed", "Couldn't install it. Try again.");
        application_packages_drop_enable(false);
        break;
    case StoreCommandKind::link_cancel:
        application_packages_cancel();
        if (c.arg != 1)
            application_packages_drop_enable(false);
        message_composer_clear(&g->link_edit);
        leave_editor();
        break;
    case StoreCommandKind::link_preview:
        application_packages_drop_enable(true);
        message_composer_clear(&g->link_edit);
        focus_editor(kStoreLinkField);
        break;
    case StoreCommandKind::protocol_answer:
        application_packages_protocol_answer(c.flag);
        break;
    case StoreCommandKind::dev_load:
        start_pick(1);
        break;
    case StoreCommandKind::dev_reload:
        if (kind == CatalogKind::plugin)
            application_catalog_dev_reload();
        else
            application_effects_reload();
        break;
    case StoreCommandKind::console_clear:
        application_dev_console_clear();
        break;
    case StoreCommandKind::request_details:
        g->details_bytes = g->changelog_bytes = 0;
        g->details_loading = false;
        g->details_slug[0] = 0;
        break;
    default:
        break;
    }
    request();
}

void dispatch(const StoreCommand& c) noexcept {
    if (c.kind == StoreCommandKind::none) {
        if (!c.flag) {
            request();
            return;
        }
        set_query({});
    }
    execute(c);
}

void act(u32 action, const UiRect& anchor) noexcept {
    StorePageState& st = page();
    const bool was_detail = st.detail;
    if (store_act(action) == StoreAct::search) {
        focus_editor(action);
        return;
    }
    if (g->editing && store_act(action) != StoreAct::dialog_confirm)
        leave_editor();
    StoreCommand c = store_page_act(g->model, &st, action, static_cast<u32>(g->scene.scroll), anchor);
    if (c.kind == StoreCommandKind::knob_set)
        c.flag = true;
    dispatch(c);
    if (was_detail && !st.detail)
        g->scene.scroll = st.saved_scroll;
    if (st.detail && !was_detail)
        g->scene.scroll = 0;
}

// ---- Layout ----------------------------------------------------------------------------------------------------
u64 sources_stamp() noexcept {
    const CatalogView& view = application_catalog_view();
    const auto theme = application_custom_theme_view();
    const PackageStatus status = application_packages_status();
    u64 h = view.revision * 0x9E3779B97F4A7C15ull;
    h ^= (static_cast<u64>(application_effects_count()) << 20) ^ static_cast<u64>(application_effects_selected_index() + 1);
    h = h * 31 + theme.revision + (theme.custom_active ? 1 : 0) + (theme.previewing ? 2 : 0);
    h = h * 31 + status.revision + static_cast<u64>(status.phase);
    h = h * 31 + (g_app.preferences.developer_mode ? 1 + 2 * static_cast<u64>(application_dev_console_revision()) : 0);
    const EffectUserSettings user = application_effects_user_settings();
    h = h * 31 + user.resolution + user.frame_rate * 7 + static_cast<u64>(user.budget) * 13 + static_cast<u64>(user.unfocused) * 17;
    // A measured cost arrives a few frames after its effect starts drawing.
    const EffectCost trial_cost = application_packages_preview_cost();
    h = h * 31 + (trial_cost.known ? 1 : 0) + (trial_cost.refused ? 2 : 0) + static_cast<u64>(application_effects_trial_state()) * 4;
    if (const i32 chosen = application_effects_selected_index(); chosen >= 0) {
        const EffectCost cost = application_effects_cost(application_effects_identity(static_cast<u32>(chosen)));
        h = h * 31 + (cost.known ? 1 : 0) + (cost.refused ? 2 : 0) + static_cast<u64>(cost.level) * 4 + static_cast<u64>(cost.milliseconds * 10.f);
    }
    return h;
}

void parse_markdown(UiMarkdown** out, u64 owner, const Utf8View& source) noexcept {
    *out = nullptr;
    if (!source.bytes)
        return;
    UiMarkdown* block = ui_markdown_bank_block(&g->bank, owner);
    if (block && ui_markdown_parse(block, source, text_size_md, ui_markdown_bank_scratch(g->bank)))
        *out = block;
}

void poll_details() noexcept {
    const StorePageState& st = page();
    if (!st.detail) {
        g->details_loading = false;
        return;
    }
    const Utf8View slug{st.slug, c_string_length(st.slug)};
    const u32 at = store_find_item(g->model, st.kind, slug);
    if (at >= g->model.count)
        return;
    const StoreItem& item = g->model.items[at];
    const CatalogView& view = application_catalog_view();
    const bool same_slug = utf8_equal({g->details_slug, c_string_length(g->details_slug)}, slug);
    if (!same_slug) {
        g->details_bytes = g->changelog_bytes = 0;
        g->details_loading = false;
        copy_bytes(g->details_slug, slug.data, slug.bytes);
        g->details_slug[slug.bytes] = 0;
        g->about = g->changes = nullptr;
        if (view.index && item.item != ~0u) {
            const CatalogItem& ci = view.index->items[item.item];
            if (ci.details.bytes) {
                copy_bytes(g->details_sha, ci.details.sha256, 32);
                g->details_loading = application_catalog_asset_request(CatalogAssetKind::details, ci.details);
            }
            if (ci.version_count) {
                const Utf8View text = catalog_text(*view.index, view.index->versions[ci.first_version].changelog);
                g->changelog_bytes = minimum<u32>(text.bytes, sizeof(g->changelog));
                copy_bytes(g->changelog, text.data, g->changelog_bytes);
            }
        }
        parse_markdown(&g->changes, 2, {g->changelog, g->changelog_bytes});
        g->dirty = true;
    }
    if (g->details_loading) {
        CatalogAsset asset{};
        if (application_catalog_asset_take(g->details_sha, &asset)) {
            g->details_bytes = minimum<u32>(asset.count, sizeof(g->details));
            copy_bytes(g->details, asset.bytes, g->details_bytes);
            application_catalog_asset_release(&asset);
            g->details_loading = false;
            parse_markdown(&g->about, 1, {g->details, g->details_bytes});
            g->dirty = true;
        }
    }
}

void poll_picker() noexcept {
    if (!g->picking)
        return;
    const auto status = native_file_picker_status(&g->picker);
    if (status.phase != NativeFilePickerPhase::complete)
        return;
    if (status.ok() && status.count) {
        const auto path = native_file_picker_path(&g->picker, 0);
        if (g->pick_for == 1 && page().kind == CatalogKind::plugin)
            application_catalog_sideload(path);
        else
            application_packages_begin_file(path);
        page().link_dialog = false;
    }
    native_file_picker_release(&g->picker, 0);
    g->picking = false;
    request();
}

bool media_key(const void*, u32 slot, const UiRect& rect, MediaAssetKey* key, u8* domain) noexcept {
    if (!g_app.renderer.preview_media_atlas)
        return false;
    *domain = 1;
    return store_previews_key(g->previews, slot, store_preview_pixels(ui_rect_width(rect), g_app.renderer.dpi), key);
}

// The slot whose loop plays: an uninstalled effect's open detail, else the effect card under the pointer (after
// a beat, so a pointer passing over does not flicker) or under keyboard focus. Never under reduced motion.
u32 loop_slot(u64 now, bool* pending) noexcept {
    *pending = false;
    if (g_app.preferences.reduced_motion || g->kind != CatalogKind::effect || g->scene.dialog_open)
        return 0;
    const StorePageState& st = page();
    if (st.detail) {
        const u32 at = open_item();
        return !st.knobs && at < g->model.count && !g->model.items[at].installed ? at + 1 : 0;
    }
    const auto card = [](u32 action) noexcept -> u32 {
        switch (store_act(action)) {
        case StoreAct::card:
        case StoreAct::card_primary:
        case StoreAct::card_switch:
        case StoreAct::card_settings:
        case StoreAct::card_overflow:
            return store_arg(action) < g->model.count ? store_arg(action) + 1 : 0;
        default:
            return 0;
        }
    };
    if (g->scene.focus_visible && card(g->scene.focus_action))
        return card(g->scene.focus_action);
    const u32 hover = card(g->scene.hover_action);
    if (hover != g->hover_slot) {
        g->hover_slot = hover;
        g->hover_since_ms = now;
    }
    *pending = hover && now - g->hover_since_ms < 300;
    return hover && !*pending ? hover : 0;
}

void build_and_commit(UiSettingsScene* settings) noexcept {
    StoreModel& m = g->model;
    store_model_fill(&m, {g->trying, {g->trying_name, g->trying_bytes}});
    StorePageState& st = page();
    if (st.detail)
        store_model_fill_detail(&m, st.kind, {st.slug, c_string_length(st.slug)},
                                {{g->details, g->details_bytes}, {g->changelog, g->changelog_bytes}, g->details_loading});
    m.dev_folder = application_catalog_dev_folder(st.kind == CatalogKind::plugin ? 0 : 1);
    if (!m.dev_folder.bytes)
        m.dev_folder = st.kind == CatalogKind::plugin ? utf8_literal("%LOCALAPPDATA%\\MaiCord\\plugins\\dev")
                                                      : utf8_literal("%LOCALAPPDATA%\\MaiCord\\effects\\dev");
    PackageStatus status = application_packages_status();
    StoreLink& l = m.link;
    reset_value(&l);
    l.open = status.phase != PackagePhase::idle;
    if (l.open) {
        l.phase = status.phase == PackagePhase::working ? 1 : status.phase == PackagePhase::ready ? 2 : 3;
        if (status.phase == PackagePhase::failed)
            m.error_text = utf8_literal("That link or file isn't a theme or effect MaiCord can use.");
        if (const PackageCard* card = application_packages_card(); card && status.phase == PackagePhase::ready) {
            l.kind = card->kind == PackageLinkKind::effect ? CatalogKind::effect : CatalogKind::theme;
            l.host = {card->host, c_string_length(card->host)};
            l.name = {card->name, c_string_length(card->name)};
            l.replaces = card->replaces;
            for (u32 i = 0; i < 4; ++i)
                l.swatches[i] = card->swatches[i];
            if (card->effect) {
                l.authors = {card->effect->authors, c_string_length(card->effect->authors)};
                l.license = {card->effect->license, c_string_length(card->effect->license)};
                if (card->effect->has_credit)
                    l.based_on = {card->effect->derived_from.author, c_string_length(card->effect->derived_from.author)};
            }
            l.effect_slug = {card->effect_slug, c_string_length(card->effect_slug)};
            l.has_effect = card->effect_slug[0] != 0;
            l.effect_installed = card->effect_installed;
            l.bytes = card->bytes;
            if (card->kind == PackageLinkKind::effect) {
                // The card's picture is the candidate running as the window background at the Low budget, once.
                if (!g->trial_asked) {
                    g->trial_asked = true;
                    application_packages_preview_start();
                }
                l.previewing = application_packages_previewing() && application_effects_trial_state() == EffectTrialState::drawing;
                const EffectCost cost = application_packages_preview_cost();
                l.cost_known = cost.known;
                l.cost_refused = cost.refused;
                l.cost_level = static_cast<u8>(cost.level);
                if (cost.known)
                    l.cost_text = store_cost_text(&m, cost, false);
            }
        }
    } else {
        g->trial_asked = false;
        if (st.link_dialog)
            l.phase = 0;
    }
    g->view = {message_composer_text(editor()), minimum(editor().anchor, editor().bytes), minimum(editor().caret, editor().bytes), g->editing,
               g->editing && application_window_keyboard_focus(), g->editing && application_window_keyboard_focus()};
    g->search = {&g->view, g->editing && application_window_keyboard_focus() ? &g_app.renderer.media_picker_edit_geometry : nullptr,
                 g->editing_action == kStoreLinkField ? utf8_literal("Paste an https:// link to a .maicord-theme.json or .mcfx")
                                                 : utf8_literal("Search"),
                 g->editing_action};
    g->scene.search = g->editing ? &g->search : nullptr;
    StoreLayoutInput in{&m, &st, g->scene.search, ui_rect_width(g->host_page), g_app.preferences.reduced_motion, g->about, g->changes};
    ui_store_scene_begin(&g->scene);
    store_page_build(&g->scene, in);
    UiRect modal = settings->layout.modal;
    ui_store_scene_commit(&g->scene, g->host_clip, g->host_page, modal);
}

} // namespace

// The catalog picture behind a preview tile: its still, or the loop of an effect. Called by the preview pump and
// the scene's media_key, both of which only run while the store page (and so `g`) exists.
bool store_preview_blob(u32 slot, bool loop, CatalogBlob* out) noexcept {
    const CatalogIndex* index = application_catalog_view().index;
    if (!index || !slot || slot > g->model.count)
        return false;
    const u32 at = g->model.items[slot - 1].item;
    if (at >= index->item_count)
        return false;
    const CatalogItem& item = index->items[at];
    if (loop ? !item.has_loop : !item.preview_count)
        return false;
    *out = loop ? item.loop.blob : item.previews[0].blob;
    return true;
}

bool application_store_kind(u64 page_id, CatalogKind* kind) noexcept {
    if (page_id == static_cast<u64>(SettingsPanel::plugins) + 1)
        *kind = CatalogKind::plugin;
    else if (page_id == static_cast<u64>(SettingsPanel::themes) + 1)
        *kind = CatalogKind::theme;
    else if (page_id == static_cast<u64>(SettingsPanel::effects) + 1)
        *kind = CatalogKind::effect;
    else
        return false;
    return true;
}

bool application_store_active() noexcept {
    return g && g->shown && g->scene.active;
}

void application_store_visible(bool visible, CatalogKind kind) noexcept {
    if (!visible) {
        if (g && g->shown) {
            close_menu();
            leave_editor();
            g->shown = false;
            g->scene.active = false;
            g_app.renderer.settings_store = nullptr;
            application_packages_drop_enable(false);
            // Nothing draws for a closed store: the pictures leave the atlas and a candidate effect's trial ends.
            store_previews_release(&g->previews);
            if (application_packages_previewing())
                application_packages_cancel();
            if (g->trying) {
                application_custom_theme_action(ApplicationCustomThemeAction::cancel);
                end_trial();
            }
        }
        return;
    }
    if (!ensure())
        return;
    if (g->shown && g->kind != kind) {
        close_menu();
        leave_editor();
        store_previews_release(&g->previews);
    }
    g->kind = kind;
    g->shown = true;
    g->scene.active = true;
    g->stamp = 0;
    if (!page().entered && !page().detail) {
        // The first visit opens on Installed when something is installed.
        store_model_fill(&g->model, {false, {}});
        page().tab = store_initial_tab(g->model, kind);
        page().entered = true;
    }
    set_query({page().query, page().query_bytes});
    application_catalog_browse();
    g->dirty = true;
}

bool application_store_synchronize(UiSettingsScene* settings) noexcept {
    CatalogKind kind{};
    const bool store_page = settings && settings->active && settings->selected_page.owner_id == kSettingsNativeOwner &&
                            application_store_kind(settings->selected_page.page_id, &kind);
    if (!store_page) {
        if (g && g->shown)
            application_store_visible(false, kind);
        return false;
    }
    if (!g || !g->shown || g->kind != kind)
        application_store_visible(true, kind);
    if (!g || !g->shown)
        return false;
    UiRect page_rect{}, clip{};
    if (!ui_settings_scene_host_rect(*settings, &page_rect, &clip) || !ui_rect_has_area(page_rect)) {
        g->scene.active = false;
        g_app.renderer.settings_store = nullptr;
        return false;
    }
    const u64 now = clock_monotonic_milliseconds();
    poll_details();
    poll_picker();
    if (g->trying && now - g->last_input_ms > kTrialIdleMs) {
        application_custom_theme_action(ApplicationCustomThemeAction::cancel);
        end_trial();
        g->dirty = true;
    } else if (g->trying) {
        // The trial's theme failed to load, or something else ended its preview: drop the bar.
        const auto theme = application_custom_theme_view();
        if (!theme.busy && !theme.previewing) {
            end_trial();
            g->dirty = true;
        }
    }
    const u64 stamp = sources_stamp() ^ (static_cast<u64>(settings->theme_geometry_revision) << 40);
    const bool moved = !ui_rect_equal(page_rect, g->host_page) || !ui_rect_equal(clip, g->host_clip) ||
                       !ui_rect_equal(settings->layout.modal, g->scene.modal);
    if (g->dirty || stamp != g->stamp || moved) {
        g->host_page = page_rect;
        g->host_clip = clip;
        g->stamp = stamp;
        g->dirty = false;
        g->scene.active = true;
        build_and_commit(settings);
    }
    const bool animating = ui_store_scene_tick(&g->scene, now, g_app.preferences.reduced_motion);
    if (g->scene.revision != g->revisions[0]) {
        g->revisions[0] = g->scene.revision;
        ++settings->revision;
    }
    if (g->scene.motion != g->revisions[1]) {
        g->revisions[1] = g->scene.motion;
        ++settings->motion_revision;
    }
    g->scene.media_context = g;
    g->scene.media_key = media_key;
    g_app.renderer.settings_store = &g->scene;
    // Pictures of the visible tiles; the loop frame, and the beat before a hovered card starts it, keep frames coming.
    bool beat = false;
    const u32 playing = loop_slot(now, &beat);
    const u64 due = store_previews_pump(&g->previews, {&g->scene, playing, g_app.renderer.dpi, now});
    if (g->previews.changes != g->preview_changes) {
        // The renderer queues the pictures that are resident when it lays the scene out: do that again.
        g->preview_changes = g->previews.changes;
        ++g->scene.motion;
        g->revisions[1] = g->scene.motion;
        ++settings->motion_revision;
    }
    if (g->trying || g->picking || beat || due)
        application_request_frame();
    return animating || beat || due != 0;
}

// ---- Input -----------------------------------------------------------------------------------------------------
namespace {

UiSettingsTarget host_target(const UiSettingsScene& scene) noexcept {
    return {UiSettingsTargetKind::host, scene.selected_page, 1};
}

UiScrollbarGeometry scrollbar() noexcept {
    return ui_scrollbar_geometry({g->scene.panel.left, g->scene.column.top, g->scene.panel.right, g->scene.panel.bottom}, g->scene.scroll,
                                 g->scene.content_height);
}

void handle_event(const UiStoreEvent& event, bool commit) noexcept {
    if (event.kind == UiStoreEventKind::value) {
        StoreCommand c = store_page_act(g->model, &page(), event.action, event.value, event.anchor);
        if (c.kind == StoreCommandKind::knob_set)
            c.flag = commit;
        execute(c);
        return;
    }
    if (event.kind == UiStoreEventKind::activate)
        act(event.action, event.anchor);
}

bool menu_pointer(UINT message, float x, float y) noexcept {
    const auto hit = popup_scene_hit_test(g_app.popup_scene, x, y);
    if (!hit.inside && message == WM_LBUTTONDOWN) {
        close_menu();
        application_request_frame();
        return true;
    }
    if (message == WM_MOUSEMOVE) {
        bool changed = false;
        popup_scene_set_hover(&g_app.popup_scene, x, y, &changed);
        if (changed)
            application_request_frame();
    } else if (message == WM_LBUTTONDOWN) {
        application_focus_window();
        application_capture_pointer();
        g->menu_pressed = hit.enabled ? hit.action : 0;
    } else if (message == WM_LBUTTONUP) {
        const u32 pressed = g->menu_pressed;
        const StoreMenu menu = g->menu;
        const u32 item = g->menu_item;
        g->menu_pressed = 0;
        application_release_capture();
        close_menu();
        if (hit.enabled && hit.action && pressed == hit.action)
            dispatch(store_menu_choose(g->model, &page(), menu, item, hit.action - 1));
        application_request_frame();
    }
    return true;
}

} // namespace

bool application_store_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    UiSettingsScene* active_scene = application_settings_scene();
    if (!application_store_active() || !active_scene)
        return false;
    UiSettingsScene& settings = *active_scene;
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS) {
        g->slider_action = 0;
        g->dragging_scrollbar = false;
        ui_store_scene_release_pointer(&g->scene, -1, -1, nullptr);
        return false;
    }
    if (message == WM_MOUSELEAVE) {
        if (ui_store_scene_hover(&g->scene, -1, -1))
            application_request_frame();
        return false;
    }
    if (message != WM_MOUSEMOVE && message != WM_LBUTTONDOWN && message != WM_LBUTTONUP && message != WM_LBUTTONDBLCLK &&
        message != WM_MOUSEWHEEL)
        return false;
    g->last_input_ms = clock_monotonic_milliseconds();
    float x = 0, y = 0;
    if (message == WM_MOUSEWHEEL) {
        UiPoint point{};
        if (!application_wheel_point(lparam, &point))
            return true;
        x = point.x;
        y = point.y;
    } else {
        x = client_dip_x(lparam);
        y = client_dip_y(lparam);
    }
    if (message == WM_LBUTTONDBLCLK)
        message = WM_LBUTTONDOWN;
    // A release ends the press of whoever took it: Settings' own presses (the sidebar, the close button) get theirs.
    if (message == WM_LBUTTONDOWN)
        g->pressing = false;
    else if (message == WM_LBUTTONUP) {
        const bool owned = g->pressing;
        g->pressing = false;
        if (!owned)
            return false;
    }
    if (menu_shown()) {
        if (message == WM_LBUTTONDOWN)
            g->pressing = true;
        return message == WM_MOUSEWHEEL ? true : menu_pointer(message, x, y);
    }
    const bool dialog = g->scene.dialog_open && !g->scene.dialog_bar;
    const bool in_dialog = ui_store_scene_modal_hit(g->scene, x, y);
    // The panel's body runs under Settings' close button, which stays Settings'.
    const bool inside = ui_rect_contains(g->host_clip, x, y) && !ui_rect_contains(settings.layout.close, x, y);
    if (message == WM_MOUSEWHEEL) {
        if (!inside && !dialog)
            return false;
        ui_store_scene_scroll(&g->scene, -static_cast<float>(static_cast<short>(HIWORD(wparam))) / 120.f * 56.f);
        application_request_frame();
        return true;
    }
    if (message == WM_MOUSEMOVE) {
        if (g->dragging_scrollbar) {
            const auto geometry = scrollbar();
            ui_store_scene_scroll_to(&g->scene, ui_scrollbar_scroll_from_pointer(geometry, y, g->scrollbar_grab));
            application_request_frame();
            return true;
        }
        if (g->slider_action) {
            UiStoreEvent event{};
            if (ui_store_scene_slider(&g->scene, g->slider_action, x, &event))
                handle_event(event, false);
            return true;
        }
        const bool changed = ui_store_scene_hover(&g->scene, inside || in_dialog ? x : -1, inside || in_dialog ? y : -1);
        if (changed)
            application_request_frame();
        return dialog;
    }
    if (message == WM_LBUTTONDOWN) {
        if (!inside && !in_dialog && !dialog)
            return false;
        g->pressing = true;
        if (dialog && !in_dialog)
            return true;
        ui_settings_scene_focus(&settings, host_target(settings));
        application_focus_window();
        if (!dialog) {
            const auto geometry = scrollbar();
            if (ui_scrollbar_thumb_hit(geometry, x, y)) {
                g->dragging_scrollbar = true;
                g->scrollbar_grab = y - geometry.thumb.top;
                application_capture_pointer();
                return true;
            }
        }
        application_capture_pointer();
        UiStoreEvent event{};
        ui_store_scene_press(&g->scene, x, y, &event);
        if (ui_store_scene_press_is_slider(g->scene)) {
            g->slider_action = g->scene.press_action;
            if (event.kind == UiStoreEventKind::value)
                handle_event(event, false);
        }
        if (g->editing && !(g->scene.press_action && store_act(g->scene.press_action) == StoreAct::search))
            leave_editor();
        application_request_frame();
        return true;
    }
    // WM_LBUTTONUP
    const u32 slider = g->slider_action;
    g->slider_action = 0;
    const bool scrollbar = g->dragging_scrollbar;
    g->dragging_scrollbar = false;
    // Take the release before giving up the capture: the WM_CAPTURECHANGED that ReleaseCapture sends cancels the
    // press it still finds.
    UiStoreEvent event{};
    if (!scrollbar)
        ui_store_scene_release_pointer(&g->scene, x, y, &event);
    application_release_capture();
    if (scrollbar)
        return true;
    if (slider) {
        // Commit the value the drag ended on.
        UiStoreEvent end{};
        end.kind = UiStoreEventKind::value;
        end.action = slider;
        end.value = ui_store_scene_value(g->scene, slider);
        handle_event(end, true);
        return true;
    }
    if (event.kind == UiStoreEventKind::activate) {
        if (store_act(event.action) == StoreAct::search && g->query_edit.bytes && event.action != kStoreLinkField &&
            ui_rect_contains({event.anchor.right - 32, event.anchor.top, event.anchor.right, event.anchor.bottom}, x, y)) {
            set_query({});
            request();
            return true;
        }
        handle_event(event, false);
    }
    return true;
}

bool application_store_character(u32 code) noexcept {
    if (!application_store_active())
        return false;
    if (g->swallow_char) {
        g->swallow_char = false;
        return true;
    }
    if (!g->editing || code < 0x20 || code == 0x7f)
        return g->scene.dialog_open && !g->scene.dialog_bar;
    auto& model = editor();
    if (!model.history)
        message_composer_enable_history(&model, 2048);
    message_composer_insert_utf16(&model, static_cast<u16>(code), clock_monotonic_milliseconds());
    query_changed();
    return true;
}

bool application_store_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    UiSettingsScene* active_scene = application_settings_scene();
    if (!application_store_active() || !active_scene)
        return false;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return false;
    UiSettingsScene& settings = *active_scene;
    g->last_input_ms = clock_monotonic_milliseconds();
    const bool control = application_control_down(), shift = application_shift_down(), alt = application_alt_down();
    if (menu_shown()) {
        bool changed = false;
        if (code == VK_ESCAPE || code == VK_LEFT || code == VK_RIGHT) {
            close_menu();
            application_request_frame();
        } else if (code == VK_UP || code == VK_DOWN || code == VK_TAB) {
            popup_scene_move_selection(&g_app.popup_scene, code == VK_UP || (code == VK_TAB && shift) ? -1 : 1, &changed);
            application_request_frame();
        } else if ((code == VK_RETURN || code == VK_SPACE) && !repeated) {
            const u32 action = popup_scene_selected_action(g_app.popup_scene);
            const StoreMenu menu = g->menu;
            const u32 item = g->menu_item;
            close_menu();
            if (action)
                dispatch(store_menu_choose(g->model, &page(), menu, item, action - 1));
        }
        return true;
    }
    const bool dialog = g->scene.dialog_open && !g->scene.dialog_bar;
    const bool mine = dialog || ui_settings_scene_focused(settings).kind == UiSettingsTargetKind::host;
    if (code == 'F' && control && !repeated && (mine || ui_settings_scene_focus_zone(settings) == UiSettingsFocusZone::panel)) {
        ui_settings_scene_focus(&settings, host_target(settings));
        if (!dialog)
            focus_editor(store_action(StoreAct::search));
        return true;
    }
    if (g->editing) {
        auto& model = editor();
        if (code == VK_ESCAPE) {
            if (model.bytes) {
                message_composer_clear(&model);
                query_changed();
            } else
                leave_editor();
            return true;
        }
        if (code == VK_RETURN || code == VK_TAB || code == VK_DOWN) {
            if (g->editing_action == kStoreLinkField && code == VK_RETURN) {
                act(store_action(StoreAct::dialog_confirm), {});
                return true;
            }
            leave_editor();
            if (code == VK_RETURN)
                return true;
            if (code == VK_TAB) {
                UiStoreEvent event{};
                ui_store_scene_key(&g->scene, shift ? UiStoreKey::back_tab : UiStoreKey::tab, &event);
                return true;
            }
        } else {
            if (!model.history)
                message_composer_enable_history(&model, 2048);
            MessageComposerResult result = MessageComposerResult::unchanged;
            if (control && (code == 'C' || code == 'X')) {
                const u32 first = minimum(model.anchor, model.caret), last = maximum(model.anchor, model.caret);
                if (last > first && clipboard_write_utf8(g_app.window, {model.text + first, last - first}) && code == 'X')
                    result = message_composer_replace_range(&model, first, last, {});
            } else if (control && code == 'V') {
                char text[kPackageUrlMaximumBytes];
                const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
                u32 kept = 0;
                while (read.ok() && kept < read.bytes) {
                    const auto scalar = utf8_decode_one(text + kept, read.bytes - kept);
                    if (!scalar.valid || scalar.codepoint < 0x20 || scalar.codepoint == 0x7f)
                        break;
                    kept += scalar.width;
                }
                if (kept)
                    result = message_composer_insert_utf8(&model, {text, kept});
            } else if (!message_composer_edit_key(&model, code, control, shift, &result))
                return true;
            if (result == MessageComposerResult::changed)
                query_changed();
            else
                request();
            return true;
        }
    }
    if (code == VK_ESCAPE) {
        bool handled = false;
        const StoreCommand c = store_page_escape(g->model, &page(), &handled);
        if (handled) {
            dispatch(c);
            return true;
        }
        return false;
    }
    if (alt && code == VK_LEFT && page().detail) {
        act(store_action(StoreAct::back), {});
        return true;
    }
    if (code == VK_OEM_2 && !control && !alt && mine && !dialog) {
        ui_settings_scene_focus(&settings, host_target(settings));
        focus_editor(store_action(StoreAct::search));
        g->swallow_char = true;
        return true;
    }
    if (!mine)
        return false;
    UiStoreKey key;
    switch (code) {
    case VK_TAB:
        key = shift ? UiStoreKey::back_tab : UiStoreKey::tab;
        break;
    case VK_LEFT:
        key = UiStoreKey::left;
        break;
    case VK_RIGHT:
        key = UiStoreKey::right;
        break;
    case VK_UP:
        key = UiStoreKey::up;
        break;
    case VK_DOWN:
        key = UiStoreKey::down;
        break;
    case VK_HOME:
        key = control ? UiStoreKey::first : UiStoreKey::home;
        break;
    case VK_END:
        key = control ? UiStoreKey::last : UiStoreKey::end;
        break;
    case VK_PRIOR:
        key = UiStoreKey::page_up;
        break;
    case VK_NEXT:
        key = UiStoreKey::page_down;
        break;
    case VK_RETURN:
        key = UiStoreKey::enter;
        break;
    case VK_SPACE:
        key = UiStoreKey::space;
        break;
    case VK_APPS:
        key = UiStoreKey::menu;
        break;
    case VK_F10:
        if (!shift)
            return false;
        key = UiStoreKey::menu;
        break;
    default:
        return false;
    }
    if ((key == UiStoreKey::enter || key == UiStoreKey::space) && repeated)
        return true;
    UiStoreEvent event{};
    if (key == UiStoreKey::menu) {
        const u32 focused = ui_store_scene_focused(g->scene);
        const StoreAct what = store_act(focused);
        if (what == StoreAct::card || what == StoreAct::card_primary || what == StoreAct::card_switch || what == StoreAct::row_switch ||
            what == StoreAct::row_primary || what == StoreAct::row_settings || what == StoreAct::row_overflow || what == StoreAct::card_overflow) {
            const u32 item = store_arg(focused);
            UiRect anchor{};
            ui_store_scene_rect(g->scene, focused, &anchor);
            if (item < g->model.count && g->model.items[item].installed)
                open_menu(StoreMenu::overflow, item, anchor);
        }
        return true;
    }
    const bool used = ui_store_scene_key(&g->scene, key, &event);
    if (event.kind != UiStoreEventKind::none)
        handle_event(event, true);
    if (used)
        application_request_frame();
    return used;
}

bool application_store_open(PackageLinkKind kind) noexcept {
    if (!ensure() || !application_settings_open())
        return false;
    const SettingsPanel panel = kind == PackageLinkKind::effect ? SettingsPanel::effects
                                : kind == PackageLinkKind::theme ? SettingsPanel::themes
                                                                 : SettingsPanel::plugins;
    PackageDeepLink link{};
    if (application_packages_take_deep_link(&link)) {
        StorePageState& target = g->pages[static_cast<u32>(link.kind)];
        const u32 n = c_string_length(link.slug);
        copy_bytes(target.slug, link.slug, n);
        target.slug[n] = 0;
        target.detail = true;
        target.knobs = false;
        target.detail_from = StoreTab::browse;
        target.entered = true;
        g->scene.scroll = 0;
    }
    return application_settings_select_page({kSettingsNativeOwner, static_cast<u64>(panel) + 1}) && application_settings_synchronize();
}

bool application_store_open_effect_knobs(const Utf8View& slug) noexcept {
    if (!slug.bytes || slug.bytes > kCatalogSlugMaximumBytes || !ensure())
        return false;
    StorePageState& target = g->pages[static_cast<u32>(CatalogKind::effect)];
    copy_bytes(target.slug, slug.data, slug.bytes);
    target.slug[slug.bytes] = 0;
    target.detail = target.knobs = target.entered = true;
    target.detail_from = StoreTab::installed;
    target.tab = StoreTab::installed;
    g->scene.scroll = 0;
    return application_settings_select_page({kSettingsNativeOwner, static_cast<u64>(SettingsPanel::effects) + 1});
}

void application_store_release() noexcept {
    if (!g)
        return;
    close_menu();
    if (g->picking) {
        native_file_picker_cancel(&g->picker);
        native_file_picker_release(&g->picker, 0);
    }
    if (g->trying)
        application_custom_theme_action(ApplicationCustomThemeAction::cancel);
    store_previews_release(&g->previews);
    ui_markdown_bank_release(&g->bank);
    g_app.renderer.settings_store = nullptr;
    ui_store_scene_release(&g->scene);
    VirtualFree(g, 0, MEM_RELEASE);
    g = nullptr;
}

} // namespace maicord::app_detail
