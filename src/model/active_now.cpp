#include "maicord/model/active_now.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kWorkCards = 64;

const VoiceState* voice_of(const VoiceStateStore* store, u64 user_id) noexcept {
    if (!store || !store->records.data)
        return nullptr;
    for (u32 i = 0; i < store->count && i < store->records.count; ++i)
        if (store->records[i].user_id == user_id && store->records[i].channel_id)
            return &store->records[i];
    return nullptr;
}
// Points into *set, which receives the user's activities.
const UserActivity* first_activity(const ActiveNowInput& input, u64 user_id, UserActivitySet* set) noexcept {
    if (!input.activities || !input.activities(input.context, user_id, set))
        return nullptr;
    for (u32 i = 0; i < set->count && i < kUserActivityMaximum; ++i)
        if (set->items[i].type != UserActivityType::custom &&
            user_activity_text(set->items[i], UserActivityField::name).bytes)
            return &set->items[i];
    return nullptr;
}
u64 started(const UserActivity* activity) noexcept {
    return !activity ? 0 : activity->start_ms ? activity->start_ms : activity->created_ms;
}
void copy_text(char* out, u16* bytes, Utf8View text) noexcept {
    u32 take = minimum<u32>(text.bytes, kActiveNowNameBytes);
    while (take && take < text.bytes && (static_cast<u8>(text.data[take]) & 0xC0) == 0x80)
        --take; // keep whole code points
    copy_bytes(out, text.data, take);
    *bytes = static_cast<u16>(take);
}
void describe(ActiveNowCard& card, const UserActivity& activity) noexcept {
    copy_text(card.name, &card.name_bytes, user_activity_text(activity, UserActivityField::name));
    auto detail = user_activity_text(activity, UserActivityField::details);
    if (!detail.bytes)
        detail = user_activity_text(activity, UserActivityField::state);
    copy_text(card.detail, &card.detail_bytes, detail);
    card.activity_type = activity.type;
}
bool same_name(const ActiveNowCard& card, Utf8View name) noexcept {
    return card.name_bytes == name.bytes && bytes_equal(card.name, name.data, name.bytes);
}
void add_member(ActiveNowCard& card, u64 user_id, u64 start) noexcept {
    if (card.member_count < kActiveNowMaximumMembers)
        card.members[card.member_count++] = user_id;
    ++card.total;
    if (start && (!card.started_ms || start < card.started_ms))
        card.started_ms = start;
}
// Larger groups first; then the activity running longest; then identity.
bool before(const ActiveNowCard& a, const ActiveNowCard& b) noexcept {
    if (a.total != b.total)
        return a.total > b.total;
    if (a.kind != b.kind)
        return a.kind == ActiveNowKind::voice;
    const u64 sa = a.started_ms ? a.started_ms : ~0ull, sb = b.started_ms ? b.started_ms : ~0ull;
    if (sa != sb)
        return sa < sb;
    const u64 ka = a.kind == ActiveNowKind::voice ? a.channel_id : a.application_id;
    const u64 kb = b.kind == ActiveNowKind::voice ? b.channel_id : b.application_id;
    if (ka != kb)
        return ka < kb;
    return a.members[0] < b.members[0];
}
} // namespace

u32 active_now_project(const ActiveNowInput& input, Slice<ActiveNowCard> output) noexcept {
    if ((!input.friends.data && input.friends.count) || (!output.data && output.count))
        return 0;
    ActiveNowCard work[kWorkCards];
    u32 count = 0;
    for (u32 f = 0; f < input.friends.count; ++f) {
        const u64 user = input.friends[f];
        if (!user)
            continue;
        bool duplicate = false;
        for (u32 g = 0; g < f && !duplicate; ++g)
            duplicate = input.friends[g] == user;
        if (duplicate)
            continue;
        const auto* voice = voice_of(input.voice, user);
        UserActivitySet activities;
        const auto* activity = first_activity(input, user, &activities);
        if (!voice && !activity)
            continue;
        ActiveNowCard* card = nullptr;
        for (u32 c = 0; c < count && !card; ++c) {
            auto& candidate = work[c];
            if (voice) {
                if (candidate.kind == ActiveNowKind::voice && candidate.channel_id == voice->channel_id &&
                    candidate.guild_id == voice->guild_id)
                    card = &candidate;
            } else if (candidate.kind == ActiveNowKind::activity && candidate.activity_type == activity->type &&
                       (activity->application_id
                            ? candidate.application_id == activity->application_id
                            : !candidate.application_id &&
                                  same_name(candidate, user_activity_text(*activity, UserActivityField::name))))
                card = &candidate;
        }
        if (!card) {
            if (count == kWorkCards)
                continue;
            card = &work[count++];
            clear_bytes(card, static_cast<u32>(sizeof(*card)));
            if (voice) {
                card->kind = ActiveNowKind::voice;
                card->guild_id = voice->guild_id;
                card->channel_id = voice->channel_id;
            } else {
                card->kind = ActiveNowKind::activity;
                card->application_id = activity->application_id;
                describe(*card, *activity);
            }
        }
        if (voice && activity && !card->name_bytes)
            describe(*card, *activity);
        add_member(*card, user, started(activity));
    }
    // Insertion sort: the working set is small and the order must be stable.
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j > 0 && before(work[j], work[j - 1]); --j) {
            ActiveNowCard swap = work[j];
            work[j] = work[j - 1];
            work[j - 1] = swap;
        }
    const u32 emitted = minimum(count, minimum<u32>(output.count, kActiveNowMaximumCards));
    for (u32 i = 0; i < emitted; ++i)
        output[i] = work[i];
    return emitted;
}

bool active_now_equal(ConstSlice<ActiveNowCard> a, ConstSlice<ActiveNowCard> b) noexcept {
    if (a.count != b.count)
        return false;
    for (u32 i = 0; i < a.count; ++i)
        if (!bytes_equal(&a[i], &b[i], static_cast<u32>(sizeof(ActiveNowCard))))
            return false;
    return true;
}
} // namespace maicord
