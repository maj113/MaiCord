// Included inside the audit settings implementation namespace.
struct AuditAction {
    Utf8View verb; // Empty: the type has no entry.
    // 0 none, 1 user, 2 channel, 3 role.
    u8 target;
};
// The known action types, as three parallel tables: kAuditTypes[i] has the target kAuditTargets[i] and the i-th
// verb of kAuditVerbs (NUL-terminated, back to back). Pointer-free: a table of verb views costs a relocation each.
constexpr u16 kAuditTypes[]{
    1, 10, 11, 12, 13, 14, 15, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61,
    62, 72, 73, 74, 75, 80, 82, 90, 92, 100, 101, 102, 110, 111, 112, 140, 141, 142, 143};
constexpr u8 kAuditTargets[]{
    0, 2, 2, 0, 2, 2, 2, 1, 0, 1, 1, 1, 1, 0, 0, 1, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr u32 kAuditActionCount = sizeof(kAuditTypes) / sizeof(kAuditTypes[0]);
static_assert(sizeof(kAuditTargets) == kAuditActionCount);
constexpr char kAuditVerbs[] =
    "updated the server\0"
    "created channel\0"
    "updated channel\0"
    "deleted a channel\0"
    "added permissions in\0"
    "changed permissions in\0"
    "removed permissions in\0"
    "removed\0"
    "pruned inactive members\0"
    "banned\0"
    "revoked the ban for\0"
    "updated\0"
    "changed the roles of\0"
    "moved members in voice\0"
    "disconnected members from voice\0"
    "added the app\0"
    "created role\0"
    "updated role\0"
    "deleted a role\0"
    "created an invite\0"
    "updated an invite\0"
    "revoked an invite\0"
    "created a webhook\0"
    "updated a webhook\0"
    "deleted a webhook\0"
    "added an emoji\0"
    "updated an emoji\0"
    "deleted an emoji\0"
    "deleted a message from\0"
    "bulk deleted messages\0"
    "pinned a message from\0"
    "unpinned a message from\0"
    "added an integration\0"
    "removed an integration\0"
    "added a sticker\0"
    "deleted a sticker\0"
    "created an event\0"
    "updated an event\0"
    "cancelled an event\0"
    "created a thread\0"
    "updated a thread\0"
    "deleted a thread\0"
    "created an AutoMod rule\0"
    "updated an AutoMod rule\0"
    "deleted an AutoMod rule\0"
    "had a message blocked by AutoMod\0";
// Filter choices after "All actions": common moderation and setup changes.
constexpr u16 kAuditFilters[]{1, 10, 11, 12, 14, 20, 22, 23, 24, 25, 30, 31, 32, 40, 42, 72};
AuditAction audit_action(u16 type) noexcept {
    const char* verb = kAuditVerbs;
    for (u32 i = 0; i < kAuditActionCount; ++i) {
        u32 bytes = 0;
        while (verb[bytes])
            ++bytes;
        if (kAuditTypes[i] == type)
            return {{verb, bytes}, kAuditTargets[i]};
        verb += bytes + 1;
    }
    return {};
}
