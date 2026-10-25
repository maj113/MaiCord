#include "maicord/render/content_text_retention.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/background_presentation.hpp"
#include <stddef.h>

namespace maicord {
u64 content_text_theme_signature(const Theme& theme) noexcept {
    // All foreground tokens precede the separately painted background. Motion
    // is not text paint and carries padding, so the hash stops before it.
    static_assert(offsetof(Theme, motion) == 20 * sizeof(Color) + sizeof(UiThemeTokens) + sizeof(UiThemeScale));
    static_assert(offsetof(Theme, background) == offsetof(Theme, motion) + sizeof(UiThemeMotion));
    const u64 foreground = hash64_counted_bytes(kHash64Seed, &theme, offsetof(Theme, motion));
    // Dark authored role colors change policy when a gradient becomes active,
    // even with identical caller token values. Secondary ink always uses its token.
    // Palette edits and nonzero intensity changes do not change that policy.
    const bool gradient_ink = ui_background_active(theme);
    const auto policy = hash64_counted_bytes(foreground, &theme.appearance, sizeof(theme.appearance));
    return hash64_counted_bytes(policy, &gradient_ink, sizeof(gradient_ink));
}

bool content_text_retention_probe(ContentTextRetentionOwner* owner,
                                  const ContentTextRetentionKey& key) noexcept {
    if (!owner) {
        return false;
    }
    if (content_text_retention_base_current(*owner, key) &&
        owner->key.popup_signature == key.popup_signature) {
        saturating_increment(owner->hits);
        return true;
    }
    saturating_increment(owner->misses);
    return false;
}

void content_text_retention_commit(ContentTextRetentionOwner* owner,
                                   const ContentTextRetentionKey& key) noexcept {
    if (!owner) {
        return;
    }
    owner->key = key;
    owner->valid = true;
    saturating_increment(owner->builds);
}

void content_text_retention_invalidate(ContentTextRetentionOwner* owner) noexcept {
    if (!owner) {
        return;
    }
    if (owner->valid) {
        saturating_increment(owner->invalidations);
    }
    reset_value<ContentTextRetentionKey>(&owner->key);
    owner->valid = false;
    ++owner->generation;
}

void content_text_retention_reset(ContentTextRetentionOwner* owner) noexcept {
    if (owner) {
        const u32 generation = owner->generation + 1u;
        reset_value<ContentTextRetentionOwner>(owner);
        owner->generation = generation;
    }
}

} // namespace maicord
