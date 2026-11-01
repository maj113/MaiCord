#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
struct Theme;

struct ContentTextRetentionKey {
    u64 shell_signature;
    u64 message_signature;
    u64 popup_signature;
    u64 theme_signature;
    u64 selection_message_id;
    u64 selection_command_identity;
    u32 selection_revision;
    u32 selection_command_index;
    u32 selection_anchor;
    u32 selection_caret;
    u32 dpi;
    u32 atlas_evicted_entries;
    u32 atlas_full_resets;
    bool include_messages;
};

struct ContentTextRetentionOwner {
    ContentTextRetentionKey key;
    u32 builds;
    u32 hits;
    u32 misses;
    u32 invalidations;
    // Advances on every invalidate/reset: a retained shell prefix recorded at
    // one generation is stale once anything else invalidated the stream.
    u32 generation;
    bool valid;
};

// Only the overlay signature may differ when reusing the shell/message prefix.
// Glyph atlas mutations invalidate every retained quad, including that prefix.
inline bool content_text_retention_base_current(const ContentTextRetentionOwner& owner,
                                                const ContentTextRetentionKey& key) noexcept {
    const auto& left=owner.key;
    return owner.valid && left.shell_signature==key.shell_signature &&
        left.message_signature==key.message_signature && left.theme_signature==key.theme_signature &&
        left.selection_message_id==key.selection_message_id &&
        left.selection_command_identity==key.selection_command_identity &&
        left.selection_revision==key.selection_revision && left.selection_command_index==key.selection_command_index &&
        left.selection_anchor==key.selection_anchor && left.selection_caret==key.selection_caret &&
        left.dpi==key.dpi && left.atlas_evicted_entries==key.atlas_evicted_entries &&
        left.atlas_full_resets==key.atlas_full_resets && left.include_messages==key.include_messages;
}

u64 content_text_theme_signature(const Theme& theme) noexcept;
bool content_text_retention_probe(ContentTextRetentionOwner* owner,
                                  const ContentTextRetentionKey& key) noexcept;
void content_text_retention_commit(ContentTextRetentionOwner* owner,
                                   const ContentTextRetentionKey& key) noexcept;
void content_text_retention_invalidate(ContentTextRetentionOwner* owner) noexcept;
void content_text_retention_reset(ContentTextRetentionOwner* owner) noexcept;

static_assert(sizeof(ContentTextRetentionKey) == 80);
static_assert(sizeof(ContentTextRetentionOwner) == 104);

} // namespace maicord
