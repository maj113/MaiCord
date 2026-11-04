#include "store_source.hpp"
#include "../effects/effects.hpp"
#include "../packages/packages.hpp"
#include "../plugins/catalog_service.hpp"
#include "../plugins/dev_console.hpp"
#include "../settings/custom_theme.hpp"
#include "../shell/state.hpp"
#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/consent.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord::app_detail {
namespace {

struct Writer {
    StoreModel* m;
    Utf8View add(const Utf8View& a, const Utf8View& b = {}, const Utf8View& c = {}, const Utf8View& d = {}) noexcept {
        const u32 bytes = a.bytes + b.bytes + c.bytes + d.bytes;
        if (m->text_used + bytes > sizeof(m->text))
            return {};
        char* out = m->text + m->text_used;
        u32 at = 0;
        const Utf8View parts[4]{a, b, c, d};
        for (const Utf8View& part : parts) {
            if (part.bytes)
                copy_bytes(out + at, part.data, part.bytes);
            at += part.bytes;
        }
        m->text_used += bytes;
        return {out, bytes};
    }
    Utf8View c_text(const char* text) noexcept { return add({text, c_string_length(text)}); }
};

void hex_bytes(char* out, const u8* bytes, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        out[i * 2] = ascii_hex_digit(bytes[i] >> 4);
        out[i * 2 + 1] = ascii_hex_digit(bytes[i]);
    }
}

// The first four and the last three bytes of a SHA-256 as hex around an ellipsis.
Utf8View hex_sha(Writer& w, const u8 (&sha)[32]) noexcept {
    char head[8], tail[6];
    hex_bytes(head, sha, 4);
    hex_bytes(tail, sha + 29, 3);
    return w.add({head, 8}, utf8_literal("\xE2\x80\xA6"), {tail, 6});
}

PluginBinding* find_binding(u64 owner) noexcept {
    PluginRuntime* runtime = application_catalog_runtime();
    for (u32 i = 0; runtime && i < runtime->binding_count; ++i)
        if (runtime->bindings[i] && runtime->bindings[i]->owner_id == owner)
            return runtime->bindings[i];
    return nullptr;
}

StoreItem* add_item(StoreModel* m) noexcept {
    if (m->count >= kStoreItemMaximum)
        return nullptr;
    StoreItem* item = &m->items[m->count++];
    reset_value(item);
    item->item = item->library = item->file = ~0u;
    return item;
}

void apply_state(Writer& w, const CatalogView& view, StoreItem* out, const CatalogIndex* index) noexcept {
    const CatalogItemState st = application_catalog_item_state(out->kind, out->slug);
    if (st.installed) {
        out->installed = true;
        out->order = view.state ? static_cast<u32>(st.installed - view.state->entries) : 0;
        out->installed_version = catalog_entry_version(*st.installed);
        out->previous_version = {st.installed->previous_version, st.installed->previous_version_bytes};
        out->sha = hex_sha(w, st.installed->sha256);
        if (out->kind == CatalogKind::plugin) {
            if (const PluginBinding* binding = find_binding(st.installed->owner_id))
                out->has_settings = binding->image.descriptor && binding->image.descriptor->setting_count != 0;
        }
    }
    out->enabled = st.enabled;
    out->auto_update = st.auto_update;
    out->sideloaded = st.sideloaded;
    out->unreviewed = st.unreviewed;
    out->can_roll_back = st.can_roll_back;
    out->incompatible = st.incompatible;
    out->revoked = static_cast<u8>(st.revoked);
    out->reason = st.reason;
    out->installable = view.installable && !st.incompatible && st.revoked == CatalogRevocationSeverity::none;
    if (index && st.update != kCatalogNone && st.update < index->version_count) {
        const CatalogVersion& v = index->versions[st.update];
        out->update = true;
        out->needs_review = st.needs_review;
        out->update_version = catalog_text(*index, v.text);
        out->changelog = catalog_text(*index, v.changelog);
    }
    const auto& op = view.operation;
    if (op.slug_bytes && op.kind == out->kind && op.slug_bytes == out->slug.bytes &&
        bytes_equal(op.slug, out->slug.data, op.slug_bytes)) {
        switch (op.phase) {
        case CatalogPhase::queued:
        case CatalogPhase::downloading:
            out->busy = 1;
            break;
        case CatalogPhase::verifying:
            out->busy = 2;
            break;
        case CatalogPhase::consent:
        case CatalogPhase::applying:
            out->busy = 3;
            break;
        default:
            break;
        }
    }
}

} // namespace

Utf8View store_cost_text(StoreModel* m, const EffectCost& cost, bool here) noexcept {
    Writer w{m};
    const u32 tenths = static_cast<u32>(cost.milliseconds * 10.f + .5f);
    Utf8View head = cost.refused ? utf8_literal("Too heavy") : cost.level == EffectCostClass::light ? utf8_literal("Light")
                    : cost.level == EffectCostClass::medium ? utf8_literal("Medium") : utf8_literal("Heavy");
    char digits[16];
    u32 n = decimal_write(digits, tenths / 10);
    digits[n++] = '.';
    digits[n++] = static_cast<char>('0' + tenths % 10);
    return w.add(head, here ? utf8_literal(" here \xC2\xB7 ") : utf8_literal(" \xC2\xB7 "), Utf8View{digits, n}, utf8_literal(" ms"));
}

// The WCAG 2 level a theme's text keeps over its background.
Utf8View store_contrast_text(const u32 (&swatches)[4]) noexcept {
    const float ratio = ui_theme_contrast_ratio(color_from_rgb24(swatches[2]), color_from_rgb24(swatches[0]));
    return ratio >= 7.f ? utf8_literal("WCAG AAA for text") : ratio >= 4.5f ? utf8_literal("WCAG AA for text") : utf8_literal("Below WCAG AA for text");
}

void store_model_fill(StoreModel* m, const StoreSourceHooks& hooks) noexcept {
    m->count = 0;
    m->text_used = 0;
    Writer w{m};
    const CatalogView& view = application_catalog_view();
    const CatalogIndex* index = view.index;
    m->developer_mode = g_app.preferences.developer_mode;
    m->console_count = m->console_total = 0;
    if (m->developer_mode) {
        // The newest lines of the Developer console, oldest first.
        const u32 held = application_dev_console_count();
        const u32 shown = held < kStoreConsoleLines ? held : kStoreConsoleLines;
        m->console_total = held;
        for (u32 i = 0; i < shown; ++i) {
            const DevLine* line = application_dev_console_line(held - shown + i);
            if (!line)
                break;
            const u32 s = line->second_of_day;
            const char stamp[8]{static_cast<char>('0' + s / 36000), static_cast<char>('0' + s / 3600 % 10), ':',
                                static_cast<char>('0' + s / 60 % 60 / 10), static_cast<char>('0' + s / 60 % 10), ':',
                                static_cast<char>('0' + s % 60 / 10), static_cast<char>('0' + s % 10)};
            StoreConsoleLine& out = m->console[m->console_count++];
            out.kind = static_cast<u8>(line->kind);
            out.time = w.add({stamp, 8});
            out.name = w.add({line->name, line->name_bytes});
            out.text = w.add({line->text, line->text_bytes});
        }
    }
    m->installable = view.installable;
    m->checked_unix = view.checked_unix;
    m->fetched_unix = view.fetched_unix;
    m->catalog = view.status == CatalogStatus::not_configured                                                   ? StoreCatalogState::not_configured
                 : view.status == CatalogStatus::ok                                                             ? StoreCatalogState::ready
                 : view.status == CatalogStatus::stale                                                          ? StoreCatalogState::stale
                 : (view.status == CatalogStatus::unreachable || view.status == CatalogStatus::invalid) && !view.fetching ? StoreCatalogState::unreachable
                                                                                                                : StoreCatalogState::loading;
    m->error_text = view.error != CatalogError::none ? application_catalog_error_text(view.error) : Utf8View{};
    m->auto_update_all = view.state && !(view.state->flags & kCatalogStateAutoUpdateOff);
    m->protocol_state = !application_packages_protocol_asked() ? 1 : 0;
    // The global rows of the Effects page's "Performance defaults" card.
    const EffectUserSettings user = application_effects_user_settings();
    StoreUniversal& global = m->global_universal;
    global = {};
    global.resolution = user.resolution;
    global.frame_rate = user.frame_rate;
    global.unfocused = static_cast<u8>(user.unfocused);
    global.budget = static_cast<u8>(user.budget);
    global.light = application_effects_light_auto() ? 1 : 2;
    // Catalog listings.
    for (u32 i = 0; index && i < index->item_count; ++i) {
        const CatalogItem& item = index->items[i];
        StoreItem* out = add_item(m);
        if (!out)
            break;
        out->kind = item.kind;
        out->item = i;
        out->slug = catalog_text(*index, item.slug);
        out->name = catalog_text(*index, item.name);
        out->summary = catalog_text(*index, item.summary);
        out->license = catalog_text(*index, item.license);
        out->source = catalog_text(*index, item.source);
        out->homepage = catalog_text(*index, item.homepage);
        Utf8View authors{};
        for (u32 a = 0; a < item.author_count; ++a)
            authors = a ? w.add(authors, utf8_literal(", "), catalog_text(*index, item.authors[a].name)) : catalog_text(*index, item.authors[a].name);
        out->authors = authors;
        out->category_count = item.category_count;
        for (u32 c = 0; c < item.category_count; ++c)
            out->categories[c] = catalog_text(*index, item.categories[c]);
        if (item.version_count) {
            const CatalogVersion& v = index->versions[item.first_version];
            out->version = catalog_text(*index, v.text);
            out->released = v.released;
            out->bytes = v.file.bytes;
            out->api_level = v.api_level;
            if (item.kind == CatalogKind::plugin)
                out->risk = store_permission_risk(v.permissions, v.host_count != 0);
            if (v.has_commit) {
                char text[8];
                hex_bytes(text, v.commit, 4);
                out->commit = w.add({text, 7});
            }
            if (!out->sha.bytes)
                out->sha = hex_sha(w, v.file.sha256);
        }
        out->cost = static_cast<u8>(item.cost);
        out->base = static_cast<u8>(item.base);
        out->has_swatches = item.has_swatches;
        for (u32 s = 0; s < 4; ++s)
            out->swatches[s] = item.swatches[s];
        if (item.has_swatches)
            out->contrast = store_contrast_text(out->swatches);
        out->effect_slug = catalog_text(*index, item.effect);
        apply_state(w, view, out, index);
    }
    // Installed items the index does not list: link installs, sideloads, items withdrawn from the catalog.
    if (view.state) {
        for (u32 i = 0; i < view.state->count; ++i) {
            const CatalogStateEntry& e = view.state->entries[i];
            const Utf8View slug = catalog_entry_slug(e);
            if (store_find_item(*m, e.kind, slug) != ~0u)
                continue;
            StoreItem* out = add_item(m);
            if (!out)
                break;
            out->kind = e.kind;
            out->slug = slug;
            out->name = slug;
            out->version = catalog_entry_version(e);
            if (e.kind == CatalogKind::plugin)
                if (const PluginBinding* binding = find_binding(e.owner_id)) {
                    out->name = w.c_text(binding->image.info.name);
                    out->authors = w.c_text(binding->image.info.author);
                    out->summary = w.c_text(binding->image.info.summary);
                    out->risk = store_permission_risk(e.approved_permissions, false);
                }
            apply_state(w, view, out, index);
        }
    }
    // The effect library: knob counts, the current background, developer packages.
    const u32 effects = application_effects_count();
    for (u32 i = 0; i < effects; ++i) {
        const EffectInfo* info = application_effects_info(i);
        if (!info)
            continue;
        const Utf8View slug = w.c_text(info->slug);
        // A development copy lists on its own, even when the catalog has an item with its slug.
        const u32 at = info->developer ? ~0u : store_find_item(*m, CatalogKind::effect, slug);
        StoreItem* out = at != ~0u ? &m->items[at] : add_item(m);
        if (!out)
            break;
        if (at == ~0u) {
            out->kind = CatalogKind::effect;
            out->slug = slug;
            out->name = w.c_text(info->name);
            out->version = w.c_text(info->version);
            out->license = w.c_text(info->license);
            out->authors = w.c_text(info->authors);
            out->summary = info->developer ? utf8_literal("Loaded from your development folder.") : utf8_literal("Installed from a link or a file.");
            out->installed = true;
            out->order = 0x10000 + i;
            out->installed_version = out->version;
            out->sideloaded = info->developer;
            out->unreviewed = !info->developer;
            out->bytes = info->bytes;
            out->enabled = true;
        }
        out->library = i;
        out->active = info->selected;
        if (!out->bytes)
            out->bytes = info->bytes;
    }
    // Installed theme files: catalog and link installs are <slug>.maicord-theme.json; any other file is a local
    // theme (imported, an edited copy, or put there by hand) and lists as installed on its own.
    const auto files = application_custom_theme_installed();
    for (u32 f = 0; f < files.count; ++f) {
        const ClientThemeCatalogEntry& entry = files[f];
        static constexpr wchar_t kSuffix[] = L".maicord-theme.json";
        constexpr u32 suffix = sizeof(kSuffix) / sizeof(wchar_t) - 1;
        StoreItem* owner = nullptr;
        for (u32 k = 0; k < m->count && !owner; ++k) {
            StoreItem& it = m->items[k];
            if (it.kind != CatalogKind::theme || !it.installed || entry.file_units != it.slug.bytes + suffix)
                continue;
            bool match = true;
            for (u32 c = 0; c < it.slug.bytes && match; ++c)
                match = entry.file[c] == static_cast<wchar_t>(it.slug.data[c]);
            for (u32 c = 0; c < suffix && match; ++c)
                match = entry.file[it.slug.bytes + c] == kSuffix[c];
            if (match)
                owner = &it;
        }
        if (!owner && (owner = add_item(m)) != nullptr) {
            char slug[17];
            u64 h = hash64_bytes(kHash64Seed, entry.file, entry.file_units * sizeof(wchar_t));
            slug[0] = '~';
            for (u32 i = 0; i < 16; ++i, h >>= 4)
                slug[1 + i] = ascii_hex_digit(h);
            owner->kind = CatalogKind::theme;
            owner->slug = w.add({slug, 17});
            owner->name = {entry.name, entry.name_bytes};
            owner->summary = utf8_literal("A theme file in your MaiCord themes folder.");
            owner->base = static_cast<u8>(entry.base);
            owner->installed = owner->enabled = owner->local = true;
            owner->order = 0x20000 + f;
        }
        if (!owner)
            break;
        owner->file = f;
        if (owner->item == ~0u && !owner->local)
            owner->name = {entry.name, entry.name_bytes}; // a link install: its name, not its slug
        if (!owner->has_swatches) {
            // The file's own colours stand in for the catalog's swatches.
            owner->has_swatches = true;
            for (u32 s = 0; s < 4; ++s)
                owner->swatches[s] = entry.preview.colors[s < entry.preview.color_count ? s : 0];
        }
    }
    // The applied theme: the installed file it came from, else the item of the same name.
    const auto theme = application_custom_theme_view();
    const u32 active_file = application_custom_theme_installed_active();
    for (u32 i = 0; i < m->count; ++i)
        if (m->items[i].kind == CatalogKind::theme && theme.custom_active &&
            (active_file != ~0u ? m->items[i].file == active_file : utf8_equal(m->items[i].name, theme.current)))
            m->items[i].active = true;
    m->trying = hooks.trying;
    m->trying_name = hooks.trying_name;
    m->theme_busy = theme.busy;
    m->theme_error = theme.can_retry_read ? 1 : theme.save_failed ? 2 : 0;
    m->theme_status = m->theme_error ? theme.status : Utf8View{};
    // A running consent.
    m->consent.open = false;
    if (view.operation.phase == CatalogPhase::consent) {
        if (const CatalogConsent* consent = application_catalog_consent()) {
            StoreConsent& k = m->consent;
            reset_value(&k);
            k.open = true;
            k.sideload = consent->sideload;
            k.update = consent->update;
            k.name = consent->request->name;
            k.version = consent->request->version;
            k.author = consent->request->author;
            k.bytes = consent->bytes;
            k.reviewed = !consent->sideload;
            const u64 permissions = consent->request->permissions;
            for (u32 bit = 0; bit < 11 && k.permission_count < 16; ++bit) {
                if (!(permissions & (u64{1} << bit)))
                    continue;
                StorePermission& p = k.permissions[k.permission_count];
                if (!store_permission_text(bit, &p))
                    continue;
                p.added = consent->update && !(consent->request->approved_permissions & (u64{1} << bit));
                ++k.permission_count;
            }
            for (u32 h = 0; h < consent->request->hosts.count && h < kStoreHostMaximum; ++h)
                k.hosts[k.host_count++] = consent->request->hosts[h];
        }
    }
}

void store_model_fill_detail(StoreModel* m, CatalogKind kind, const Utf8View& slug, const StoreDetailHooks& hooks) noexcept {
    const u32 at = store_find_item(*m, kind, slug);
    if (at == ~0u)
        return;
    const StoreItem& item = m->items[at];
    Writer w{m};
    const CatalogView& view = application_catalog_view();
    const CatalogIndex* index = view.index;
    m->details_markdown = hooks.details;
    m->details_loading = hooks.details_loading;
    m->changelog_markdown = hooks.changelog;
    // Permissions of the shown version for a plugin (the consent model, closed, carries them).
    if (!m->consent.open) {
        StoreConsent& k = m->consent;
        reset_value(&k);
        if (kind == CatalogKind::plugin && index && item.item != ~0u && index->items[item.item].version_count) {
            const CatalogVersion& v = index->versions[index->items[item.item].first_version];
            for (u32 bit = 0; bit < 11 && k.permission_count < 16; ++bit) {
                if (!(v.permissions & (u64{1} << bit)))
                    continue;
                StorePermission& p = k.permissions[k.permission_count];
                if (store_permission_text(bit, &p))
                    ++k.permission_count;
            }
            for (u32 h = 0; h < v.host_count && h < kStoreHostMaximum; ++h)
                k.hosts[k.host_count++] = catalog_text(*index, index->hosts[v.first_host + h]);
        }
    }
    // An effect's knobs: the installed package's, with values, or the catalog's declaration.
    m->knob_count = 0;
    m->effect_preview = 0;
    const McfxPackage* package = kind == CatalogKind::effect && item.library != ~0u ? application_effects_open(item.library) : nullptr;
    if (package) {
        for (u32 i = 0; i < package->knob_count && i < kStoreKnobMaximum; ++i) {
            const McfxKnob& k = package->knobs[i];
            StoreKnob& out = m->knobs[m->knob_count++];
            reset_value(&out);
            out.label = w.c_text(k.label);
            out.hint = w.c_text(k.hint);
            out.type = static_cast<u8>(k.type);
            out.percent = k.percent;
            out.minimum = k.minimum;
            out.maximum = k.maximum;
            out.fallback = k.type == McfxKnobType::color ? 0.f : k.fallback;
            McfxKnobValue value{};
            if (application_effects_knob_get(i, &value)) {
                out.value = value.number;
                out.rgb = value.rgb;
            } else {
                out.value = k.fallback;
                out.rgb = k.rgb;
            }
            out.choice_count = k.choice_count;
            for (u32 c = 0; c < k.choice_count && c < 16; ++c)
                out.choices[c] = w.c_text(package->choices[k.choice_index][c]);
        }
        const EffectUserSettings user = application_effects_user_settings();
        const EffectShaderSettings shader = application_effects_shader_settings(application_effects_identity(item.library));
        StoreUniversal& u = m->universal;
        u = {};
        u.resolution = shader.resolution;
        u.frame_rate = shader.frame_rate;
        u.speed = shader.speed;
        u.opacity = shader.opacity;
        u.blend = shader.blend;
        u.pointer = shader.pointer;
        u.unfocused = static_cast<u8>(user.unfocused);
        u.budget = static_cast<u8>(user.budget);
        u.global_resolution = user.resolution;
        u.global_frame_rate = user.frame_rate;
        u.requested_frame_rate = package->requested_frame_rate;
        u.pointer_available = (package->uses & effect_use_pointer) != 0;
        u.light = shader.light;
        u.global_light = application_effects_light_auto() ? 1 : 2;
        u.appearance = (package->uses & effect_use_appearance) != 0;
        const EffectCost cost = application_effects_cost(application_effects_identity(item.library));
        m->effect_cost_state = cost.known ? (cost.refused ? 2 : 1) : 0;
        if (cost.known)
            m->effect_cost_text = store_cost_text(m, cost, true);
        // The page previews this entry (store host): its tile is live once the entry is the effect drawing.
        const EffectTrialState preview = application_effects_preview_state(application_effects_identity(item.library));
        m->effect_preview = preview == EffectTrialState::drawing ? 1 : preview == EffectTrialState::rejected ? 2 : 0;
    } else if (kind == CatalogKind::effect && index && item.item != ~0u) {
        const CatalogItem& ci = index->items[item.item];
        for (u32 i = 0; i < ci.knob_count && i < kStoreKnobMaximum; ++i) {
            const CatalogKnob& k = index->knobs[ci.first_knob + i];
            StoreKnob& out = m->knobs[m->knob_count++];
            reset_value(&out);
            out.label = catalog_text(*index, k.label);
            out.type = static_cast<u8>(k.type == CatalogKnobType::float_value ? 0 : k.type == CatalogKnobType::int_value ? 1 : k.type == CatalogKnobType::toggle ? 2 : k.type == CatalogKnobType::color ? 3 : 4);
            out.percent = k.percent;
            out.minimum = static_cast<float>(k.minimum) / 1000.f;
            out.maximum = static_cast<float>(k.maximum) / 1000.f;
            out.value = out.fallback = static_cast<float>(k.value) / 1000.f;
        }
    }
}

} // namespace maicord::app_detail
