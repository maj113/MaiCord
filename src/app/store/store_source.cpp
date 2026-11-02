#include "store_source.hpp"
#include "../effects/effects.hpp"
#include "../packages/packages.hpp"
#include "../plugins/catalog_service.hpp"
#include "../settings/custom_theme.hpp"
#include "../shell/state.hpp"
#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/consent.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::app_detail {
namespace {

struct Writer {
    StoreModel* m;
    Utf8View add(Utf8View a, Utf8View b = {}, Utf8View c = {}, Utf8View d = {}) noexcept {
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

bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && (!a.bytes || bytes_equal(a.data, b.data, a.bytes));
}

Utf8View hex_sha(Writer& w, const u8 (&sha)[32]) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    char text[16];
    u32 n = 0;
    for (u32 i = 0; i < 4; ++i) {
        text[n++] = kHex[sha[i] >> 4];
        text[n++] = kHex[sha[i] & 15];
    }
    Utf8View head = w.add({text, n});
    n = 0;
    for (u32 i = 29; i < 32; ++i) {
        text[n++] = kHex[sha[i] >> 4];
        text[n++] = kHex[sha[i] & 15];
    }
    return w.add(head, utf8_literal("\xE2\x80\xA6"), {text, n});
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
    item->item = item->library = ~0u;
    return item;
}

void apply_state(Writer& w, const CatalogView& view, StoreItem* out, const CatalogIndex* index) noexcept {
    const CatalogItemState st = application_catalog_item_state(out->kind, out->slug);
    if (st.installed) {
        out->installed = true;
        out->installed_version = catalog_entry_version(*st.installed);
        out->previous_version = {st.installed->previous_version, st.installed->previous_version_bytes};
        out->sha = hex_sha(w, st.installed->sha256);
        if (out->kind == CatalogKind::plugin) {
            if (const PluginBinding* binding = find_binding(st.installed->owner_id))
                out->has_settings = binding->image.descriptor && binding->image.descriptor->setting_count != 0;
        }
    }
    out->enabled = st.enabled;
    out->running = st.running;
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

void store_model_fill(StoreModel* m, const StoreSourceHooks& hooks) noexcept {
    m->count = 0;
    m->text_used = 0;
    Writer w{m};
    const CatalogView& view = application_catalog_view();
    const CatalogIndex* index = view.index;
    m->developer_mode = g_app.preferences.developer_mode;
    m->installable = view.installable;
    m->fetching = view.fetching;
    m->expired = view.status == CatalogStatus::stale;
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
    m->revision = view.revision;
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
                static constexpr char kHex[] = "0123456789abcdef";
                char text[8];
                for (u32 k = 0; k < 4; ++k) {
                    text[k * 2] = kHex[v.commit[k] >> 4];
                    text[k * 2 + 1] = kHex[v.commit[k] & 15];
                }
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
        out->effect_slug = catalog_text(*index, item.effect);
        apply_state(w, view, out, index);
    }
    // Installed items the index does not list: link installs, sideloads, items withdrawn from the catalog.
    if (view.state) {
        for (u32 i = 0; i < view.state->count; ++i) {
            const CatalogStateEntry& e = view.state->entries[i];
            const Utf8View slug = catalog_entry_slug(e);
            bool listed = false;
            for (u32 k = 0; k < m->count && !listed; ++k)
                listed = m->items[k].kind == e.kind && same(m->items[k].slug, slug);
            if (listed)
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
        u32 at = ~0u;
        for (u32 k = 0; k < m->count && at == ~0u; ++k)
            if (m->items[k].kind == CatalogKind::effect && same(m->items[k].slug, slug))
                at = k;
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
            out->installed_version = out->version;
            out->sideloaded = info->developer;
            out->unreviewed = !info->developer;
            out->bytes = info->bytes;
            out->enabled = true;
        }
        out->library = i;
        out->has_knobs = info->knob_count != 0;
        out->active = info->selected;
        if (!out->bytes)
            out->bytes = info->bytes;
    }
    // The applied theme.
    const auto theme = application_custom_theme_view();
    for (u32 i = 0; i < m->count; ++i)
        if (m->items[i].kind == CatalogKind::theme && theme.custom_active && same(m->items[i].name, theme.current))
            m->items[i].active = true;
    m->trying = hooks.trying;
    m->trying_name = hooks.trying_name;
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

void store_model_fill_detail(StoreModel* m, CatalogKind kind, Utf8View slug, const StoreDetailHooks& hooks) noexcept {
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
    if (kind == CatalogKind::effect && item.library != ~0u && application_effects_open(item.library)) {
        const McfxPackage* package = application_effects_open(item.library);
        for (u32 i = 0; package && i < package->knob_count && i < kStoreKnobMaximum; ++i) {
            const McfxKnob& k = package->knobs[i];
            StoreKnob& out = m->knobs[m->knob_count++];
            reset_value(&out);
            out.label = w.c_text(k.label);
            out.hint = w.c_text(k.hint);
            out.type = static_cast<u8>(k.type);
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
        const EffectCost cost = application_effects_cost(application_effects_identity(item.library));
        m->effect_cost_state = cost.known ? (cost.refused ? 2 : 1) : 0;
        if (cost.known) {
            const u32 tenths = static_cast<u32>(cost.milliseconds * 10.f + .5f);
            m->effect_cost_text = w.add(cost.refused ? utf8_literal("Too heavy here") : cost.level == EffectCostClass::light ? utf8_literal("Light here")
                                                      : cost.level == EffectCostClass::medium ? utf8_literal("Medium here") : utf8_literal("Heavy here"),
                                        utf8_literal(" \xC2\xB7 "));
            char digits[16];
            u32 n = 0;
            const u32 whole = tenths / 10;
            u32 v = whole;
            char rev[8];
            u32 r = 0;
            do {
                rev[r++] = static_cast<char>('0' + v % 10);
                v /= 10;
            } while (v);
            while (r)
                digits[n++] = rev[--r];
            digits[n++] = '.';
            digits[n++] = static_cast<char>('0' + tenths % 10);
            m->effect_cost_text = w.add(m->effect_cost_text, {digits, n}, utf8_literal(" ms"));
        }
    } else if (kind == CatalogKind::effect && index && item.item != ~0u) {
        const CatalogItem& ci = index->items[item.item];
        for (u32 i = 0; i < ci.knob_count && i < kStoreKnobMaximum; ++i) {
            const CatalogKnob& k = index->knobs[ci.first_knob + i];
            StoreKnob& out = m->knobs[m->knob_count++];
            reset_value(&out);
            out.label = catalog_text(*index, k.label);
            out.type = static_cast<u8>(k.type == CatalogKnobType::float_value ? 0 : k.type == CatalogKnobType::int_value ? 1 : k.type == CatalogKnobType::toggle ? 2 : k.type == CatalogKnobType::color ? 3 : 4);
            out.minimum = static_cast<float>(k.minimum) / 1000.f;
            out.maximum = static_cast<float>(k.maximum) / 1000.f;
            out.value = out.fallback = static_cast<float>(k.value) / 1000.f;
        }
    }
}

} // namespace maicord::app_detail
