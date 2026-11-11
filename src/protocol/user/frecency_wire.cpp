#include "frecency_wire.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord::protocol_detail {
namespace {

void push_recent(FrecencyItem& item, u64 value) noexcept {
    if (!value)
        return;
    // Keep the newest samples in ascending order.
    if (item.recent_count == kFrecencyRecentUses) {
        if (value <= item.recent_uses[0])
            return;
        copy_bytes(item.recent_uses, item.recent_uses + 1, (kFrecencyRecentUses - 1) * sizeof(u64));
        --item.recent_count;
    }
    u32 at = item.recent_count++;
    while (at && item.recent_uses[at - 1] > value) {
        item.recent_uses[at] = item.recent_uses[at - 1];
        --at;
    }
    item.recent_uses[at] = value;
}

bool decode_item(const ProtoSource& w, u32 p, u32 stop, FrecencyItem& item) noexcept {
    reset_value(&item);
    while (p < stop) {
        u32 f = 0, t = 0;
        u64 n = 0;
        if (!proto_tag(w, p, stop, f, t))
            return false;
        if (f == 1 || f == 3 || f == 4) {
            if (t != 0 || !proto_varint(w, p, stop, n))
                return false;
            if (f == 1)
                item.total_uses = static_cast<u32>(n);
            else if (f == 3)
                item.frecency = static_cast<i32>(n);
            else
                item.score = static_cast<i32>(n);
        } else if (f == 2 && t == 2) {
            u32 end = 0;
            if (!proto_length(w, p, stop, end))
                return false;
            while (p < end) {
                if (!proto_varint(w, p, end, n))
                    return false;
                push_recent(item, n);
            }
        } else if (f == 2 && t == 0) {
            if (!proto_varint(w, p, stop, n))
                return false;
            push_recent(item, n);
        } else if (!proto_skip(w, p, stop, t, f)) {
            return false;
        }
    }
    return true;
}

void write_item(ProtoWriter& out, const FrecencyItem& item) noexcept {
    if (item.total_uses)
        out.scalar(1, item.total_uses);
    u32 packed = 0;
    for (u32 i = 0; i < item.recent_count; ++i)
        packed += proto_varint_size(item.recent_uses[i]);
    if (packed) {
        out.tag(2, 2);
        out.varint(packed);
        for (u32 i = 0; i < item.recent_count; ++i)
            out.varint(item.recent_uses[i]);
    }
    // int32 negatives are sign-extended to ten bytes on the wire.
    if (item.frecency)
        out.scalar(3, static_cast<u64>(static_cast<i64>(item.frecency)));
    if (item.score)
        out.scalar(4, static_cast<u64>(static_cast<i64>(item.score)));
}

} // namespace

FrecencyWire frecency_entry_decode(const ProtoSource& w, u32 p, u32 stop, Utf8View* key, FrecencyItem* item) noexcept {
    reset_value(key);
    reset_value(item);
    while (p < stop) {
        u32 f = 0, t = 0;
        if (!proto_tag(w, p, stop, f, t))
            return FrecencyWire::protobuf;
        if ((f == 1 || f == 2) && t == 2) {
            u32 end = 0;
            if (!proto_length(w, p, stop, end))
                return FrecencyWire::protobuf;
            if (f == 1) {
                *key = {reinterpret_cast<const char*>(w.raw.data + p), end - p};
                if (!utf8_validate(*key).valid)
                    return FrecencyWire::utf8;
            } else if (!decode_item(w, p, end, *item)) {
                return FrecencyWire::protobuf;
            }
            p = end;
        } else if (!proto_skip(w, p, stop, t, f)) {
            return FrecencyWire::protobuf;
        }
    }
    return FrecencyWire::ok;
}

void frecency_entry_write(ProtoWriter& out, const Utf8View& key, const FrecencyItem& item) noexcept {
    ProtoWriter counter{};
    write_item(counter, item);
    const u32 value = counter.at;
    const u32 entry = 1 + proto_varint_size(key.bytes) + key.bytes + 1 + proto_varint_size(value) + value;
    out.tag(1, 2);
    out.varint(entry);
    out.string(1, key);
    out.tag(2, 2);
    out.varint(value);
    write_item(out, item);
}

bool settings_data_version(const ProtoSource& w, u32 p, u32 end, u32* version) noexcept {
    while (p < end) {
        u32 f = 0, t = 0;
        u64 n = 0;
        if (!proto_tag(w, p, end, f, t))
            return false;
        if (f == 3 && t == 0) {
            if (!proto_varint(w, p, end, n) || n > ~u32{0})
                return false;
            *version = static_cast<u32>(n);
        } else if (!proto_skip(w, p, end, t, f)) {
            return false;
        }
    }
    return true;
}

} // namespace maicord::protocol_detail
