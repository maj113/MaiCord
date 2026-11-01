// Shared by the chat-state and custom-emoji translation units in their private namespaces.
// Backs the first `required` of `capacity` units of one backing (bank `bank`) before use;
// a store without a backing callback is fully backed.
bool back_range(const ChatStateStore& store, ChatStateBacking backing, u32 bank, u32& backed, u32 capacity,
                u32 required) noexcept {
    if (required <= backed)
        return true;
    if (required > capacity)
        return false;
    if (!store.backing_reserve)
        return true;
    if (!store.backing_reserve(store.backing_context, backing, bank, required))
        return false;
    backed = required;
    return true;
}
