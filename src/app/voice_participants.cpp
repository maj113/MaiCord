#include "maicord/app/voice_participants.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
template <typename T> bool reserve(T*& data, u32& capacity, u32 count, u32 required) noexcept {
    if (required <= capacity)
        return true;
    constexpr u32 maximum = ~u32{0} / sizeof(T);
    if (required > maximum)
        return false;
    u32 next = capacity ? capacity : 4096 / sizeof(T);
    while (next < required)
        next = next > maximum / 2 ? maximum : next * 2;
    auto* replacement = static_cast<T*>(VirtualAlloc(nullptr, static_cast<usize>(next) * sizeof(T),
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!replacement)
        return false;
    copy_bytes(replacement, data, count * static_cast<u32>(sizeof(T)));
    if (data)
        VirtualFree(data, 0, MEM_RELEASE);
    data = replacement;
    capacity = next;
    return true;
}
template <typename T, typename Key>
u32 position(const T* items, u32 count, Key T::*field, Key key) noexcept {
    u32 first = 0, end = count;
    while (first < end) {
        const u32 middle = first + (end - first) / 2;
        if (items[middle].*field < key)
            first = middle + 1;
        else
            end = middle;
    }
    return first;
}
VoiceParticipant& ensure_user(VoiceParticipants& participants, u64 id) noexcept {
    const u32 at = position(participants.users, participants.user_count, &VoiceParticipant::user_id, id);
    if (at == participants.user_count || participants.users[at].user_id != id) {
        copy_bytes(participants.users + at + 1, participants.users + at,
                   (participants.user_count - at) * static_cast<u32>(sizeof(VoiceParticipant)));
        participants.users[at] = {};
        participants.users[at].user_id = id;
        ++participants.user_count;
    }
    return participants.users[at];
}
struct CountUsers {
    const VoiceParticipants* participants;
    u32 missing;
};
bool count_user(void* context, u64 id) noexcept {
    auto& count = *static_cast<CountUsers*>(context);
    if (!voice_participants_user(*count.participants, id).user_id)
        ++count.missing;
    return true;
}
bool connect_user(void* context, u64 id) noexcept {
    ensure_user(*static_cast<VoiceParticipants*>(context), id).connected = true;
    return true;
}
struct StageSources {
    VoiceSsrcBinding* sources;
    u64 user_id;
    u32 count;
};
bool add_source(StageSources& stage, u32 ssrc, u32 primary, VoiceSourceKind kind, bool active,
                u8 quality = 100) noexcept {
    if (!ssrc)
        return true;
    for (u32 i = 0; i != stage.count; ++i) {
        if (stage.sources[i].ssrc != ssrc)
            continue;
        if (stage.sources[i].kind != kind || stage.sources[i].primary_ssrc != primary)
            return false;
        stage.sources[i].active = active;
        stage.sources[i].quality = quality;
        return true;
    }
    stage.sources[stage.count++] = {stage.user_id, 0, ssrc, primary, kind, active, quality};
    return true;
}
bool stage_source(void* context, const VoiceVideoSource& source) noexcept {
    auto& stage = *static_cast<StageSources*>(context);
    return add_source(stage, source.ssrc, source.ssrc, source.kind, source.active, source.quality) &&
           (!source.ssrc ||
            add_source(stage, source.rtx_ssrc, source.ssrc, VoiceSourceKind::rtx, source.active, source.quality));
}
bool same_source(const VoiceSsrcBinding& a, const VoiceSsrcBinding& b) noexcept {
    return a.user_id == b.user_id && a.ssrc == b.ssrc && a.kind == b.kind &&
           a.primary_ssrc == b.primary_ssrc;
}
bool touches(const VoiceParticipants& participants, ConstSlice<u8> bytes) noexcept {
    return byte_ranges_overlap(bytes, {reinterpret_cast<const u8*>(&participants), sizeof(participants)}) ||
           byte_ranges_overlap(bytes,
                               {reinterpret_cast<const u8*>(participants.users),
                                participants.user_capacity * static_cast<u32>(sizeof(VoiceParticipant))}) ||
           byte_ranges_overlap(bytes,
                               {reinterpret_cast<const u8*>(participants.sources),
                                participants.source_capacity * static_cast<u32>(sizeof(VoiceSsrcBinding))});
}
VoiceParticipantsError update_sources(VoiceParticipants& participants,
                                      const VoiceParticipantEvent& event) noexcept {
    if (event.stream_count > (~u32{0} - 3) / 2)
        return VoiceParticipantsError::storage;
    const u32 maximum =
        (event.audio_ssrc ? 1u : 0u) +
        (event.opcode == 12 ? (event.stream_count ? event.stream_count * 2 : (event.video_ssrc ? 2u : 0u))
                            : 0u);
    if (participants.source_count > ~u32{0} - maximum ||
        !reserve(participants.sources, participants.source_capacity, participants.source_count,
                 participants.source_count + maximum))
        return VoiceParticipantsError::storage;
    StageSources stage{participants.sources ? participants.sources + participants.source_count : nullptr,
                       event.user_id};
    bool valid = add_source(stage, event.audio_ssrc, event.audio_ssrc, VoiceSourceKind::audio, true);
    if (event.opcode == 12) {
        if (event.stream_count)
            valid = voice_visit_participant_sources(event, stage_source, &stage) && valid;
        else {
            // Observed native legacy fallback when no stream descriptors exist.
            valid = add_source(stage, event.video_ssrc, event.video_ssrc, VoiceSourceKind::video,
                               event.video_ssrc != 0) &&
                    valid;
            if (event.video_ssrc && event.video_ssrc != ~u32{0})
                valid =
                    add_source(stage, event.video_ssrc + 1, event.video_ssrc, VoiceSourceKind::rtx, true) &&
                    valid;
        }
    }
    if (!valid) {
        clear_bytes(stage.sources, stage.count * static_cast<u32>(sizeof(VoiceSsrcBinding)));
        return VoiceParticipantsError::conflicting_sources;
    }
    u32 fresh = 0;
    for (u32 i = 0; i != stage.count; ++i) {
        const auto old = voice_participants_source(participants, stage.sources[i].ssrc);
        if (same_source(old, stage.sources[i]))
            stage.sources[i].revision = old.revision;
        else
            ++fresh;
    }
    if (participants.source_revision > ~u64{0} - fresh) {
        clear_bytes(stage.sources, stage.count * static_cast<u32>(sizeof(VoiceSsrcBinding)));
        return VoiceParticipantsError::revision_exhausted;
    }
    // Mark removals before compacting so lookups for RTX's old primary still
    // see the complete sorted old mapping. Use revision==0 temporarily as a
    // tombstone: the SSRC lookup ordering and primary metadata stay intact.
    for (u32 i = 0; i != participants.source_count; ++i) {
        auto& old = participants.sources[i];
        const auto primary = voice_participants_source(participants, old.primary_ssrc);
        bool remove = old.user_id == event.user_id &&
                      (event.opcode == 12 || old.kind == VoiceSourceKind::audio ||
                       (old.kind == VoiceSourceKind::rtx && primary.kind == VoiceSourceKind::audio &&
                        primary.ssrc != event.audio_ssrc));
        for (u32 j = 0; j != stage.count && !remove; ++j) {
            const auto& next = stage.sources[j];
            remove =
                old.ssrc == next.ssrc || (old.primary_ssrc == next.ssrc && !same_source(primary, next));
        }
        if (remove)
            old.revision = 0;
    }
    u32 keep = 0;
    const u32 before = participants.source_count;
    for (u32 i = 0; i != before; ++i)
        if (participants.sources[i].revision)
            participants.sources[keep++] = participants.sources[i];
    for (u32 i = 0; i != stage.count; ++i) {
        auto next = stage.sources[i];
        if (!next.revision)
            next.revision = ++participants.source_revision;
        participants.sources[keep++] = next;
    }
    if (before + stage.count != keep)
        clear_bytes(participants.sources + keep,
                    (before + stage.count - keep) * static_cast<u32>(sizeof(VoiceSsrcBinding)));
    participants.source_count = keep;
    heap_sort(
        participants.sources, keep,
        [](const VoiceSsrcBinding& a, const VoiceSsrcBinding& b) noexcept { return a.ssrc < b.ssrc; });
    return VoiceParticipantsError::none;
}
} // namespace
VoiceParticipant voice_participants_user(const VoiceParticipants& participants, u64 id) noexcept {
    const u32 at = position(participants.users, participants.user_count, &VoiceParticipant::user_id, id);
    return at < participants.user_count && participants.users[at].user_id == id ? participants.users[at]
                                                                                : VoiceParticipant{};
}
VoiceSsrcBinding voice_participants_source(const VoiceParticipants& participants, u32 ssrc) noexcept {
    const u32 at = position(participants.sources, participants.source_count, &VoiceSsrcBinding::ssrc, ssrc);
    return at < participants.source_count && participants.sources[at].ssrc == ssrc
               ? participants.sources[at]
               : VoiceSsrcBinding{};
}
VoiceParticipantsResult voice_participants_event(VoiceParticipants* participants, u64 generation, u8 opcode,
                                                 JsonValue data, Slice<u32> scratch,
                                                 u32 maximum_depth) noexcept {
    if (!participants || !generation || scratch.count > ~u32{0} / sizeof(u32))
        return {VoiceParticipantsError::invalid_input};
    if (participants->generation && participants->generation != generation)
        return {VoiceParticipantsError::stale_generation};
    if (touches(*participants, {reinterpret_cast<const u8*>(data.source.data), data.source.bytes}) ||
        touches(*participants,
                {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))}))
        return {VoiceParticipantsError::invalid_input};
    VoiceParticipantEvent event{};
    const auto decoded = voice_decode_participant(opcode, data, scratch, maximum_depth, &event);
    if (!decoded.ok())
        return {VoiceParticipantsError::decode, decoded};
    CountUsers count{participants};
    if (opcode == 11 && !voice_visit_participant_users(event, count_user, &count))
        return {VoiceParticipantsError::decode};
    const u32 extra = opcode == 11 ? count.missing
                                   : (event.user_id && opcode != 13 &&
                                              !voice_participants_user(*participants, event.user_id).user_id
                                          ? 1
                                          : 0);
    if (participants->user_count > ~u32{0} - extra ||
        !reserve(participants->users, participants->user_capacity, participants->user_count,
                 participants->user_count + extra))
        return {VoiceParticipantsError::storage};
    if (opcode == 11) {
        // The complete immutable array has already validated; reserve for the
        // missing IDs before publishing any membership changes. Duplicates need
        // no temporary array and existing-only announcements need no growth.
        if (!voice_visit_participant_users(event, connect_user, participants))
            return {VoiceParticipantsError::decode};
    } else if (opcode == 13) {
        const u32 at = position(participants->users, participants->user_count, &VoiceParticipant::user_id,
                                event.user_id);
        if (at < participants->user_count && participants->users[at].user_id == event.user_id) {
            copy_bytes(participants->users + at, participants->users + at + 1,
                       (--participants->user_count - at) * static_cast<u32>(sizeof(VoiceParticipant)));
            participants->users[participants->user_count] = {};
        }
        u32 keep = 0;
        for (u32 i = 0; i != participants->source_count; ++i)
            if (participants->sources[i].user_id != event.user_id)
                participants->sources[keep++] = participants->sources[i];
        if (participants->source_count != keep)
            clear_bytes(participants->sources + keep,
                        (participants->source_count - keep) * static_cast<u32>(sizeof(VoiceSsrcBinding)));
        participants->source_count = keep;
    } else if (event.user_id) {
        if (opcode == 5 || opcode == 12) {
            const auto error = update_sources(*participants, event);
            if (error != VoiceParticipantsError::none)
                return {error};
        }
        auto& member = ensure_user(*participants, event.user_id);
        if (opcode == 5)
            member.speaking = event.value;
        if (opcode == 18) {
            member.flags = event.value;
            member.has_flags = true;
        }
        if (opcode == 20) {
            member.platform = event.value;
            member.has_platform = true;
        }
    }
    participants->generation = generation;
    return {};
}
void voice_participants_clear_sources(VoiceParticipants* participants) noexcept {
    if (!participants)
        return;
    clear_bytes(participants->sources,
                participants->source_count * static_cast<u32>(sizeof(VoiceSsrcBinding)));
    participants->source_count = 0;
    for (u32 i = 0; i != participants->user_count; ++i)
        participants->users[i].speaking = 0;
}
void voice_participants_release(VoiceParticipants* participants) noexcept {
    if (!participants)
        return;
    if (participants->users)
        VirtualFree(participants->users, 0, MEM_RELEASE);
    if (participants->sources)
        VirtualFree(participants->sources, 0, MEM_RELEASE);
    *participants = {};
}
} // namespace maicord
