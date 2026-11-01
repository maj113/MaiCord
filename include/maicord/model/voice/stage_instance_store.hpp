#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
// Live stages (stage instances) by channel: topic and privacy level.
inline constexpr u32 kStageInstanceCapacity = 128;
inline constexpr u32 kStageInstanceTopicBytes = 480; // 120 code points.

struct StageInstance {
    u64 id, guild_id, channel_id;
    u16 topic_bytes;
    u8 privacy_level;
    u8 reserved[5];
    char topic[kStageInstanceTopicBytes];
};
struct StageInstanceStore {
    StageInstance records[kStageInstanceCapacity];
    u32 count, revision, dropped;
};

void stage_instance_store_reset(StageInstanceStore*) noexcept;
// Creates or replaces the channel's instance (a zero id removes it). False
// for invalid input or when full (counted in dropped).
bool stage_instance_store_apply(StageInstanceStore*, u64 id, u64 guild_id, u64 channel_id, u8 privacy_level,
                                Utf8View topic) noexcept;
bool stage_instance_store_remove(StageInstanceStore*, u64 channel_id) noexcept;
bool stage_instance_store_clear_guild(StageInstanceStore*, u64 guild_id) noexcept;
const StageInstance* stage_instance_store_find(const StageInstanceStore&, u64 channel_id) noexcept;
inline Utf8View stage_instance_topic(const StageInstance& instance) noexcept {
    return {instance.topic, instance.topic_bytes};
}
} // namespace maicord
