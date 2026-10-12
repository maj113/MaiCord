#include "maicord/app/mls_tree.hpp"

namespace maicord {
u32 mls_tree_level(u32 node) noexcept {
    if (mls_tree_is_leaf(node))
        return 0;
    u32 level = 0;
    while (level < 31 && (node & (1u << level)))
        ++level;
    return level;
}
bool mls_tree_is_below(u32 node, u32 ancestor) noexcept {
    const u32 node_level = mls_tree_level(node), ancestor_level = mls_tree_level(ancestor);
    if (node_level > ancestor_level)
        return false;
    if (ancestor_level >= 31)
        return true;
    return (node >> (ancestor_level + 1)) == (ancestor >> (ancestor_level + 1));
}
u32 mls_tree_left(u32 node) noexcept {
    const u32 level = mls_tree_level(node);
    return level ? node ^ (1u << (level - 1)) : node;
}
u32 mls_tree_right(u32 node) noexcept {
    const u32 level = mls_tree_level(node);
    return level ? node ^ (3u << (level - 1)) : node;
}
MlsStatus mls_tree_parent(u32 node, u32* parent) noexcept {
    const u32 level = mls_tree_level(node);
    if (!parent || level >= 31)
        return MlsStatus::invalid_argument;
    *parent = (node | (1u << level)) & ~(1u << (level + 1));
    return MlsStatus::ok;
}
MlsStatus mls_tree_sibling(u32 node, u32 ancestor, u32* sibling) noexcept {
    if (!sibling || node == ancestor || !mls_tree_is_below(node, ancestor))
        return MlsStatus::invalid_argument;
    const u32 left = mls_tree_left(ancestor);
    *sibling = mls_tree_is_below(node, left) ? mls_tree_right(ancestor) : left;
    return MlsStatus::ok;
}
MlsStatus mls_tree_node_count(u32 leaf_count, u32* node_count) noexcept {
    if (!node_count || leaf_count > 0x80000000u)
        return MlsStatus::invalid_argument;
    *node_count = leaf_count ? 2 * (leaf_count - 1) + 1 : 0;
    return MlsStatus::ok;
}
MlsStatus mls_tree_full_leaf_count(u32 leaf_count, u32* full_leaf_count) noexcept {
    if (!full_leaf_count || leaf_count > 0x80000000u)
        return MlsStatus::invalid_argument;
    u32 value = 1;
    while (value < leaf_count)
        value <<= 1;
    *full_leaf_count = value;
    return MlsStatus::ok;
}
MlsStatus mls_tree_root(u32 leaf_count, u32* root) noexcept {
    u32 nodes = 0;
    if (!root || !leaf_count || mls_tree_node_count(leaf_count, &nodes) != MlsStatus::ok)
        return MlsStatus::invalid_argument;
    u32 log = 0;
    while (nodes >> (log + 1))
        ++log;
    *root = (1u << log) - 1;
    return MlsStatus::ok;
}
MlsStatus mls_tree_leaf_ancestor(u32 left_leaf, u32 right_leaf, u32* ancestor) noexcept {
    if (!ancestor || left_leaf > 0x7fffffffu || right_leaf > 0x7fffffffu)
        return MlsStatus::invalid_argument;
    u32 left = left_leaf * 2, right = right_leaf * 2, shifts = 0;
    if (left == right) {
        *ancestor = left;
        return MlsStatus::ok;
    }
    while (left != right) {
        left >>= 1;
        right >>= 1;
        ++shifts;
    }
    *ancestor = (left << shifts) + (1u << (shifts - 1)) - 1;
    return MlsStatus::ok;
}
namespace {
MlsStatus walk(u32 node, u32 leaf_count, Slice<u32> path, u32* count, bool copath) noexcept {
    if (!count || (!path.data && path.count))
        return MlsStatus::invalid_argument;
    *count = 0;
    u32 nodes = 0, root = 0;
    if (mls_tree_node_count(leaf_count, &nodes) != MlsStatus::ok || !leaf_count || node >= nodes ||
        mls_tree_root(leaf_count, &root) != MlsStatus::ok)
        return MlsStatus::invalid_argument;
    u32 current = node, used = 0;
    while (current != root) {
        if (used >= path.count)
            return MlsStatus::buffer_too_small;
        u32 parent = 0;
        if (mls_tree_parent(current, &parent) != MlsStatus::ok)
            return MlsStatus::invalid_argument;
        if (copath) {
            if (const auto status = mls_tree_sibling(current, parent, &path.data[used]);
                status != MlsStatus::ok)
                return status;
        } else
            path.data[used] = parent;
        ++used;
        current = parent;
    }
    *count = used;
    return MlsStatus::ok;
}
} // namespace
MlsStatus mls_tree_direct_path(u32 node, u32 leaf_count, Slice<u32> path, u32* count) noexcept {
    return walk(node, leaf_count, path, count, false);
}
MlsStatus mls_tree_copath(u32 node, u32 leaf_count, Slice<u32> path, u32* count) noexcept {
    return walk(node, leaf_count, path, count, true);
}
} // namespace maicord
