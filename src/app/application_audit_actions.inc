// Included inside the audit settings implementation namespace.
struct AuditAction {
    Utf8View verb;
    u16 type;
    // 0 none, 1 user, 2 channel, 3 role.
    u8 target;
    constexpr AuditAction(u16 type, Utf8View verb, u8 target) noexcept
        : verb(verb), type(type), target(target) {}
};
static_assert(sizeof(AuditAction)==24);
constexpr AuditAction kAuditActions[]{
    {1, utf8_literal("updated the server"), 0},
    {10, utf8_literal("created channel"), 2},
    {11, utf8_literal("updated channel"), 2},
    {12, utf8_literal("deleted a channel"), 0},
    {13, utf8_literal("added permissions in"), 2},
    {14, utf8_literal("changed permissions in"), 2},
    {15, utf8_literal("removed permissions in"), 2},
    {20, utf8_literal("removed"), 1},
    {21, utf8_literal("pruned inactive members"), 0},
    {22, utf8_literal("banned"), 1},
    {23, utf8_literal("revoked the ban for"), 1},
    {24, utf8_literal("updated"), 1},
    {25, utf8_literal("changed the roles of"), 1},
    {26, utf8_literal("moved members in voice"), 0},
    {27, utf8_literal("disconnected members from voice"), 0},
    {28, utf8_literal("added the app"), 1},
    {30, utf8_literal("created role"), 3},
    {31, utf8_literal("updated role"), 3},
    {32, utf8_literal("deleted a role"), 0},
    {40, utf8_literal("created an invite"), 0},
    {41, utf8_literal("updated an invite"), 0},
    {42, utf8_literal("revoked an invite"), 0},
    {50, utf8_literal("created a webhook"), 0},
    {51, utf8_literal("updated a webhook"), 0},
    {52, utf8_literal("deleted a webhook"), 0},
    {60, utf8_literal("added an emoji"), 0},
    {61, utf8_literal("updated an emoji"), 0},
    {62, utf8_literal("deleted an emoji"), 0},
    {72, utf8_literal("deleted a message from"), 1},
    {73, utf8_literal("bulk deleted messages"), 0},
    {74, utf8_literal("pinned a message from"), 1},
    {75, utf8_literal("unpinned a message from"), 1},
    {80, utf8_literal("added an integration"), 0},
    {82, utf8_literal("removed an integration"), 0},
    {90, utf8_literal("added a sticker"), 0},
    {92, utf8_literal("deleted a sticker"), 0},
    {100, utf8_literal("created an event"), 0},
    {101, utf8_literal("updated an event"), 0},
    {102, utf8_literal("cancelled an event"), 0},
    {110, utf8_literal("created a thread"), 0},
    {111, utf8_literal("updated a thread"), 0},
    {112, utf8_literal("deleted a thread"), 0},
    {140, utf8_literal("created an AutoMod rule"), 0},
    {141, utf8_literal("updated an AutoMod rule"), 0},
    {142, utf8_literal("deleted an AutoMod rule"), 0},
    {143, utf8_literal("had a message blocked by AutoMod"), 0},
};
// Filter choices after "All actions": common moderation and setup changes.
constexpr u16 kAuditFilters[]{1, 10, 11, 12, 14, 20, 22, 23, 24, 25, 30, 31, 32, 40, 42, 72};
const AuditAction* audit_action(u16 type) noexcept {
    for (const auto& action : kAuditActions)
        if (action.type == type)
            return &action;
    return nullptr;
}

