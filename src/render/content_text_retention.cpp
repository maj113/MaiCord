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
    static_assert(offsetof(Theme, motion) % sizeof(u64) == 0);
    // Word-at-a-time mixing: the signature is only compared in memory, and two
    // kilobytes of byte-wise FNV is a dependent multiply per byte.
    u64 foreground = kHash64Seed;
    for (u32 offset = 0; offset < offsetof(Theme, motion); offset += sizeof(u64)) {
        u64 word;
        copy_bytes(&word, reinterpret_cast<const u8*>(&theme) + offset, sizeof(word));
        foreground = (foreground ^ word) * 0x9E3779B97F4A7C15ull;
        foreground ^= foreground >> 29;
    }
    // Dark authored role colors change policy when a gradient becomes active,
    // even with identical caller token values. Secondary ink always uses its token.
    // Palette edits and nonzero intensity changes do not change that policy.
    const bool gradient_ink = ui_background_active(theme);
    const auto policy = hash64_counted_bytes(foreground, &theme.appearance, sizeof(theme.appearance));
    return hash64_counted_bytes(policy, &gradient_ink, sizeof(gradient_ink));
}

bool content_text_retention_probe(ContentTextRetentionOwner* owner,
                                  const ContentTextRetentionKey& key) noexcept {
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
    owner->key = key;
    owner->valid = true;
    saturating_increment(owner->builds);
}

void content_text_retention_invalidate(ContentTextRetentionOwner* owner) noexcept {
    if (owner->valid) {
        saturating_increment(owner->invalidations);
    }
    reset_value<ContentTextRetentionKey>(&owner->key);
    owner->valid = false;
    ++owner->generation;
}

void content_text_retention_reset(ContentTextRetentionOwner* owner) noexcept {
    const u32 generation = owner->generation + 1u;
    reset_value<ContentTextRetentionOwner>(owner);
    owner->generation = generation;
}

} // namespace maicord
