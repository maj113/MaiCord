#include "maicord/protocol/gif_favorites.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {
using E = GifFavoritesError;
struct Wire {
    ConstSlice<u8> raw;
    Utf8View encoded;
    u32 size;
    static u8 digit(char c) noexcept {
        return c >= 'A' && c <= 'Z'   ? static_cast<u8>(c - 'A')
               : c >= 'a' && c <= 'z' ? static_cast<u8>(c - 'a' + 26)
               : c >= '0' && c <= '9' ? static_cast<u8>(c - '0' + 52)
               : c == '+'             ? 62
               : c == '/'             ? 63
                                      : 0;
    }
    u8 at(u32 p) const noexcept {
        if (raw.data)
            return raw[p];
        const u32 q = p / 3 * 4;
        const u8 a = digit(encoded.data[q]), b = digit(encoded.data[q + 1]), c = digit(encoded.data[q + 2]),
                 d = digit(encoded.data[q + 3]);
        return p % 3 == 0   ? static_cast<u8>((a << 2) | (b >> 4))
               : p % 3 == 1 ? static_cast<u8>((b << 4) | (c >> 2))
                            : static_cast<u8>((c << 6) | d);
    }
};
bool varint(const Wire& w, u32& p, u32 end, u64& n) noexcept {
    n = 0;
    for (u32 i = 0; i < 10; ++i) {
        if (p == end)
            return false;
        const u8 b = w.at(p++);
        if (i == 9 && b > 1)
            return false;
        n |= static_cast<u64>(b & 127) << (i * 7);
        if (!(b & 128))
            return true;
    }
    return false;
}
bool tag(const Wire& w, u32& p, u32 end, u32& field, u32& type) noexcept {
    u64 n = 0;
    if (!varint(w, p, end, n) || n > ~u32{0} || !(n >> 3))
        return false;
    field = static_cast<u32>(n >> 3);
    type = static_cast<u32>(n & 7);
    return true;
}
bool length(const Wire& w, u32& p, u32 end, u32& stop) noexcept {
    u64 n = 0;
    if (!varint(w, p, end, n) || n > end - p)
        return false;
    stop = p + static_cast<u32>(n);
    return true;
}
bool skip(const Wire& w, u32& p, u32 end, u32 type, u32 field, u32 depth = 0) noexcept {
    u64 n = 0;
    if (type == 0)
        return varint(w, p, end, n);
    if (type == 1 || type == 5) {
        const u32 bytes = type == 1 ? 8 : 4;
        if (bytes > end - p)
            return false;
        p += bytes;
        return true;
    }
    if (type == 2) {
        u32 stop = 0;
        if (!length(w, p, end, stop))
            return false;
        p = stop;
        return true;
    }
    if (type != 3 || depth >= 16)
        return false;
    while (p < end) {
        u32 f = 0, t = 0;
        if (!tag(w, p, end, f, t))
            return false;
        if (t == 4)
            return f == field;
        if (!skip(w, p, end, t, f, depth + 1))
            return false;
    }
    return false;
}
struct Entry {
    u32 begin, end, key_begin, key_end, src_begin, src_end;
    u32 width, height, order;
    GifFavoriteFormat format;
};
bool utf8(const Wire& w, u32 begin, u32 end) noexcept {
    while (begin < end) {
        char scalar[4]{};
        const u32 available = minimum(4u, end - begin);
        for (u32 i = 0; i < available; ++i)
            scalar[i] = static_cast<char>(w.at(begin + i));
        const auto decoded = utf8_decode_one(scalar, available);
        if (!decoded.valid)
            return false;
        begin += decoded.width;
    }
    return true;
}
E value(const Wire& w, u32 p, u32 end, Entry& entry) noexcept {
    while (p < end) {
        u32 f = 0, t = 0;
        u64 n = 0;
        if (!tag(w, p, end, f, t))
            return E::protobuf;
        if (f == 2) {
            u32 stop = 0;
            if (t != 2 || !length(w, p, end, stop))
                return E::protobuf;
            if (!utf8(w, p, stop))
                return E::utf8;
            entry.src_begin = p;
            entry.src_end = stop;
            p = stop;
        } else if (f == 1 || (f >= 3 && f <= 5)) {
            if (t != 0 || !varint(w, p, end, n))
                return E::protobuf;
            if (f == 1) {
                if (n > ~u32{0} && n < 0xffffffff80000000ull)
                    return E::protobuf;
                entry.format = static_cast<GifFavoriteFormat>(static_cast<i32>(n));
            } else {
                if (n > ~u32{0})
                    return E::protobuf;
                if (f == 3)
                    entry.width = static_cast<u32>(n);
                else if (f == 4)
                    entry.height = static_cast<u32>(n);
                else
                    entry.order = static_cast<u32>(n);
            }
        } else if (!skip(w, p, end, t, f))
            return E::protobuf;
    }
    return E::none;
}
E entry(const Wire& w, u32 p, u32 end, Entry& result) noexcept {
    result = {};
    result.begin = p;
    result.end = end;
    while (p < end) {
        u32 f = 0, t = 0;
        if (!tag(w, p, end, f, t))
            return E::protobuf;
        if (f == 1 || f == 2) {
            u32 stop = 0;
            if (t != 2 || !length(w, p, end, stop))
                return E::protobuf;
            if (f == 1) {
                if (!utf8(w, p, stop))
                    return E::utf8;
                result.key_begin = p;
                result.key_end = stop;
            } else {
                const E parsed = value(w, p, stop, result);
                if (parsed != E::none)
                    return parsed;
            }
            p = stop;
        } else if (!skip(w, p, end, t, f))
            return E::protobuf;
    }
    return E::none;
}
E favorites(const Wire& w, u32 p, u32 end, GifFavoritesResult& result) noexcept {
    while (p < end) {
        u32 f = 0, t = 0;
        if (!tag(w, p, end, f, t))
            return E::protobuf;
        if (f == 1) {
            u32 stop = 0;
            Entry parsed{};
            if (t != 2 || !length(w, p, end, stop))
                return E::protobuf;
            if (result.entry_count == kGifFavoritesMaximumEntries)
                return E::capacity;
            const E error = entry(w, p, stop, parsed);
            if (error != E::none)
                return error;
            ++result.entry_count;
            p = stop;
        } else if (f == 2) {
            u64 n = 0;
            if (t != 0 || !varint(w, p, end, n))
                return E::protobuf;
            result.hide_tooltip = n != 0;
        } else if (!skip(w, p, end, t, f))
            return E::protobuf;
    }
    return E::none;
}
E versions(const Wire& w, u32 p, u32 end, GifFavoritesResult& result) noexcept {
    while (p < end) {
        u32 f = 0, t = 0;
        u64 n = 0;
        if (!tag(w, p, end, f, t))
            return E::protobuf;
        if (f >= 1 && f <= 3) {
            if (t != 0 || !varint(w, p, end, n) || n > ~u32{0})
                return E::protobuf;
            if (f == 1)
                result.client_version = static_cast<u32>(n);
            else if (f == 2)
                result.server_version = static_cast<u32>(n);
            else
                result.data_version = static_cast<u32>(n);
        } else if (!skip(w, p, end, t, f))
            return E::protobuf;
    }
    return E::none;
}
GifFavoritesResult measure(const Wire& w) noexcept {
    GifFavoritesResult result{};
    result.proto_bytes = w.size;
    u32 p = 0;
    while (p < w.size) {
        u32 f = 0, t = 0;
        if (!tag(w, p, w.size, f, t)) {
            result.error = E::protobuf;
            return result;
        }
        if (f == 1 || f == 2) {
            u32 stop = 0;
            if (t != 2 || !length(w, p, w.size, stop)) {
                result.error = E::protobuf;
                return result;
            }
            if (f == 1) {
                result.versions_present = true;
                result.error = versions(w, p, stop, result);
            } else {
                result.group_present = true;
                if (stop - p > kGifFavoritesMaximumGroupBytes - result.group_bytes) {
                    result.error = E::capacity;
                    return result;
                }
                result.group_bytes += stop - p;
                result.error = favorites(w, p, stop, result);
            }
            if (!result.ok())
                return result;
            p = stop;
        } else if (!skip(w, p, w.size, t, f)) {
            result.error = E::protobuf;
            return result;
        }
    }
    result.index_words = gif_favorites_index_words(result.entry_count);
    return result;
}
bool encoded_wire(Utf8View input, Wire* wire, E* error) noexcept {
    if (input.bytes > base64_encoded_size(kGifFavoritesMaximumProtoBytes) ||
        !byte_range_valid({reinterpret_cast<const u8*>(input.data), input.bytes})) {
        *error = E::input;
        return false;
    }
    const auto decoded = base64_decode(input, {});
    if (!decoded.ok() && decoded.error != Base64Error::output_too_small) {
        *error = E::base64;
        return false;
    }
    *wire = {{}, input, decoded.bytes};
    return true;
}
u32 hash(const Wire& w, u32 begin, u32 end) noexcept {
    u32 n = 2166136261u;
    while (begin < end)
        n = (n ^ w.at(begin++)) * 16777619u;
    return n;
}
bool equal(const Wire& w, const Entry& a, const Entry& b) noexcept {
    if (a.key_end - a.key_begin != b.key_end - b.key_begin)
        return false;
    for (u32 i = 0; i < a.key_end - a.key_begin; ++i)
        if (w.at(a.key_begin + i) != w.at(b.key_begin + i))
            return false;
    return true;
}
bool equal(const Wire& w, const Entry& a, Utf8View b) noexcept {
    if (a.key_end - a.key_begin != b.bytes)
        return false;
    for (u32 i = 0; i < b.bytes; ++i)
        if (w.at(a.key_begin + i) != static_cast<u8>(b.data[i]))
            return false;
    return true;
}
struct Index {
    u8* bytes;
    u32 words;
    u32 get(u32 word) const noexcept {
        u32 n = 0;
        CopyMemory(&n, bytes + word * 4, 4);
        return n;
    }
    void put(u32 word, u32 n) noexcept {
        CopyMemory(bytes + word * 4, &n, 4);
    }
};
Entry indexed_entry(const Wire& group, u32 offset) noexcept {
    u32 p = offset;
    u32 end = 0;
    length(group, p, group.size, end);
    Entry result{};
    entry(group, p, end, result);
    return result;
}
// First word records first insertion position; second records the winning
// entry length-prefix offset. Full key equality resolves hash collisions.
u32 index_group(const Wire& group, Index index) noexcept {
    if (!index.words)
        return 0;
    ZeroMemory(index.bytes, index.words * 4);
    const u32 buckets = index.words / 2;
    u32 count = 0, p = 0;
    while (p < group.size) {
        u32 f = 0, t = 0;
        tag(group, p, group.size, f, t);
        if (f != 1) {
            skip(group, p, group.size, t, f);
            continue;
        }
        const u32 offset = p;
        u32 stop = 0;
        length(group, p, group.size, stop);
        Entry parsed{};
        entry(group, p, stop, parsed);
        u32 bucket = hash(group, parsed.key_begin, parsed.key_end) & (buckets - 1);
        for (u32 probes = 0; probes < buckets; ++probes) {
            const u32 previous = index.get(bucket * 2);
            if (!previous) {
                index.put(bucket * 2, offset + 1);
                ++count;
                break;
            }
            if (equal(group, parsed, indexed_entry(group, previous - 1)))
                break;
            bucket = (bucket + 1) & (buckets - 1);
        }
        index.put(bucket * 2 + 1, offset + 1);
        p = stop;
    }
    return count;
}
GifFavoriteRecord record(const Wire& group, const Entry& parsed, u32 first) noexcept {
    return {{reinterpret_cast<const char*>(group.raw.data + parsed.key_begin),
             parsed.key_end - parsed.key_begin},
            {reinterpret_cast<const char*>(group.raw.data + parsed.src_begin),
             parsed.src_end - parsed.src_begin},
            parsed.width,
            parsed.height,
            parsed.order,
            parsed.format,
            first};
}
bool comes_before(const GifFavoriteRecord& a, const GifFavoriteRecord& b) noexcept {
    return a.order > b.order || (a.order == b.order && a.first_entry_offset > b.first_entry_offset);
}
void sort_records(Slice<GifFavoriteRecord> records) noexcept {
    // In-place heap sort bounds worst-case work. Equal orders sort as a stable
    // ascending sort reversed: the later entry first.
    const auto sift = [&](u32 root, u32 end) noexcept {
        while (root < end / 2) {
            u32 child = root * 2 + 1;
            if (child + 1 < end && comes_before(records[child], records[child + 1]))
                ++child;
            if (!comes_before(records[root], records[child]))
                break;
            const auto tmp = records[root];
            records[root] = records[child];
            records[child] = tmp;
            root = child;
        }
    };
    for (u32 i = records.count / 2; i; --i)
        sift(i - 1, records.count);
    for (u32 end = records.count; end > 1; --end) {
        const auto tmp = records[0];
        records[0] = records[end - 1];
        records[end - 1] = tmp;
        sift(0, end - 1);
    }
}
void record_index(ConstSlice<GifFavoriteRecord> records, Index index) noexcept {
    if (!index.words)
        return;
    ZeroMemory(index.bytes, index.words * 4);
    const u32 buckets = index.words / 2;
    for (u32 i = 0; i < records.count; ++i) {
        const auto key = records[i].url;
        Wire w{{reinterpret_cast<const u8*>(key.data), key.bytes}, {}, key.bytes};
        u32 bucket = hash(w, 0, w.size) & (buckets - 1);
        for (u32 probes = 0; probes < buckets; ++probes) {
            if (!index.get(bucket * 2)) {
                index.put(bucket * 2, i + 1);
                break;
            }
            bucket = (bucket + 1) & (buckets - 1);
        }
    }
}
ConstSlice<u8> range(Slice<u8> s) noexcept {
    return {s.data, s.count};
}
template <class T> ConstSlice<u8> range(Slice<T> s) noexcept {
    return {reinterpret_cast<const u8*>(s.data), s.count * static_cast<u32>(sizeof(T))};
}
GifFavoritesResult decode(const Wire& input, Slice<u8> output, Slice<GifFavoriteRecord> records,
                          Slice<u32> scratch) noexcept {
    auto result = measure(input);
    if (!result.ok())
        return result;
    if (output.count < result.group_bytes || records.count < result.entry_count ||
        scratch.count < result.index_words) {
        result.error = E::capacity;
        return result;
    }
    // No failure remains after this admission: validated group fragments
    // concatenate in protobuf merge order, then all records fit their bank.
    u32 p = 0, copied = 0;
    while (p < input.size) {
        u32 f = 0, t = 0;
        tag(input, p, input.size, f, t);
        if (f != 2) {
            skip(input, p, input.size, t, f);
            continue;
        }
        u32 stop = 0;
        length(input, p, input.size, stop);
        while (p < stop)
            output.data[copied++] = input.at(p++);
    }
    Wire group{{output.data, result.group_bytes}, {}, result.group_bytes};
    Index index{reinterpret_cast<u8*>(scratch.data), result.index_words};
    result.record_count = index_group(group, index);
    u32 written = 0;
    for (u32 bucket = 0; bucket < index.words / 2; ++bucket) {
        const u32 first = index.get(bucket * 2), last = index.get(bucket * 2 + 1);
        if (last)
            records[written++] = record(group, indexed_entry(group, last - 1), first - 1);
    }
    sort_records({records.data, written});
    record_index({records.data, written}, index);
    return result;
}
bool outputs_valid(ConstSlice<u8> input, Slice<u8> output, Slice<GifFavoriteRecord> records,
                   Slice<u32> scratch) noexcept {
    if (records.count > kGifFavoritesMaximumEntries ||
        scratch.count > gif_favorites_index_words(kGifFavoritesMaximumEntries))
        return false;
    const ConstSlice<u8> ranges[]{input, range(output), range(records), range(scratch)};
    for (u32 i = 0; i < 4; ++i) {
        if (!byte_range_valid(ranges[i]))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (byte_ranges_overlap(ranges[i], ranges[j]))
                return false;
    }
    return true;
}
} // namespace
GifFavoritesResult gif_favorites_measure_base64(Utf8View input) noexcept {
    Wire wire{};
    GifFavoritesResult result{};
    if (!encoded_wire(input, &wire, &result.error))
        return result;
    return measure(wire);
}
GifFavoritesResult gif_favorites_decode_base64(Utf8View input, Slice<u8> output,
                                               Slice<GifFavoriteRecord> records,
                                               Slice<u32> scratch) noexcept {
    const ConstSlice<u8> bytes{reinterpret_cast<const u8*>(input.data), input.bytes};
    Wire wire{};
    GifFavoritesResult result{};
    if (!outputs_valid(bytes, output, records, scratch)) {
        result.error = E::input;
        return result;
    }
    if (!encoded_wire(input, &wire, &result.error))
        return result;
    return decode(wire, output, records, scratch);
}
const GifFavoriteRecord* gif_favorites_find(ConstSlice<GifFavoriteRecord> records, ConstSlice<u32> scratch,
                                            Utf8View url) noexcept {
    if (!records.data || !records.count || records.count > kGifFavoritesMaximumEntries ||
        scratch.count < 4 || scratch.count > gif_favorites_index_words(kGifFavoritesMaximumEntries) ||
        (scratch.count & (scratch.count - 1)) || !scratch.data ||
        !byte_range_valid({reinterpret_cast<const u8*>(records.data),
                           records.count * static_cast<u32>(sizeof(GifFavoriteRecord))}) ||
        !byte_range_valid({reinterpret_cast<const u8*>(scratch.data), scratch.count * 4}) ||
        !byte_range_valid({reinterpret_cast<const u8*>(url.data), url.bytes}))
        return nullptr;
    Wire w{{reinterpret_cast<const u8*>(url.data), url.bytes}, {}, url.bytes};
    const u32 buckets = scratch.count / 2;
    u32 bucket = hash(w, 0, w.size) & (buckets - 1);
    for (u32 probes = 0; probes < buckets; ++probes) {
        const u32 slot = scratch[bucket * 2];
        if (!slot || slot > records.count)
            return nullptr;
        if (utf8_equal(records[slot - 1].url, url))
            return records.data + slot - 1;
        bucket = (bucket + 1) & (buckets - 1);
    }
    return nullptr;
}
namespace {
struct BinaryWriter {
    u8* data;
    u32 used;
    void bytes(const u8* source, u32 count) noexcept {
        if (data && count)
            copy_bytes(data + used, source, count);
        used += count;
    }
    void number(u64 n) noexcept {
        do {
            const u8 b = static_cast<u8>(n & 127);
            n >>= 7;
            if (data)
                data[used] = b | (n ? 128 : 0);
            ++used;
        } while (n);
    }
    void scalar(u32 field, u64 n) noexcept {
        number(field * 8);
        number(n);
    }
    void string(u32 field, Utf8View text) noexcept {
        number(field * 8 + 2);
        number(text.bytes);
        bytes(reinterpret_cast<const u8*>(text.data), text.bytes);
    }
};
struct MutationPlan {
    u32 winning_offset, order, records;
    bool found, set_hide_tooltip;
};
void changed_value(const Wire& group, const Entry* observed, const GifFavoriteMutation& mutation, u32 order,
                   BinaryWriter& out) noexcept {
    if (observed) {
        u32 p = observed->begin;
        while (p < observed->end) {
            u32 f = 0, t = 0;
            tag(group, p, observed->end, f, t);
            if (f != 2) {
                skip(group, p, observed->end, t, f);
                continue;
            }
            u32 stop = 0;
            length(group, p, observed->end, stop);
            while (p < stop) {
                const u32 begin = p;
                u32 vf = 0, vt = 0;
                tag(group, p, stop, vf, vt);
                skip(group, p, stop, vt, vf);
                const bool replaced =
                    mutation.kind == GifFavoriteMutationKind::add ? vf >= 1 && vf <= 5 : vf == 5;
                if (!replaced)
                    out.bytes(group.raw.data + begin, p - begin);
            }
        }
    }
    if (mutation.kind == GifFavoriteMutationKind::add) {
        out.scalar(1, static_cast<u32>(mutation.format));
        out.string(2, mutation.src);
        out.scalar(3, mutation.width);
        out.scalar(4, mutation.height);
    }
    out.scalar(5, order);
}
void changed_entry(const Wire& group, const Entry* observed, const GifFavoriteMutation& mutation, u32 order,
                   BinaryWriter& out) noexcept {
    if (observed) {
        u32 p = observed->begin;
        while (p < observed->end) {
            const u32 begin = p;
            u32 f = 0, t = 0;
            tag(group, p, observed->end, f, t);
            skip(group, p, observed->end, t, f);
            if (f != 2)
                out.bytes(group.raw.data + begin, p - begin);
        }
    } else
        out.string(1, mutation.url);
    BinaryWriter inner{};
    changed_value(group, observed, mutation, order, inner);
    out.number(18);
    out.number(inner.used);
    changed_value(group, observed, mutation, order, out);
}
void write_group(const Wire& group, const GifFavoriteMutation& mutation, const MutationPlan& plan,
                 BinaryWriter& out) noexcept {
    u32 p = 0;
    while (p < group.size) {
        const u32 begin = p;
        u32 f = 0, t = 0;
        tag(group, p, group.size, f, t);
        if (f == 1) {
            const u32 offset = p;
            u32 stop = 0;
            length(group, p, group.size, stop);
            Entry observed{};
            entry(group, p, stop, observed);
            p = stop;
            if (mutation.kind == GifFavoriteMutationKind::remove && equal(group, observed, mutation.url))
                continue;
            if (plan.found && offset == plan.winning_offset) {
                BinaryWriter changed{};
                changed_entry(group, &observed, mutation, plan.order, changed);
                out.number(10);
                out.number(changed.used);
                changed_entry(group, &observed, mutation, plan.order, out);
            } else
                out.bytes(group.raw.data + begin, p - begin);
        } else {
            skip(group, p, group.size, t, f);
            if (!(f == 2 && plan.set_hide_tooltip))
                out.bytes(group.raw.data + begin, p - begin);
        }
    }
    if (!plan.found && mutation.kind == GifFavoriteMutationKind::add) {
        BinaryWriter added{};
        changed_entry(group, nullptr, mutation, plan.order, added);
        out.number(10);
        out.number(added.used);
        changed_entry(group, nullptr, mutation, plan.order, out);
    }
    if (plan.set_hide_tooltip)
        out.scalar(2, 1);
}
struct PatchJson {
    Utf8View encoded;
    u32 version;
    u8 seen;
};
bool patch_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& result = *static_cast<PatchJson*>(opaque);
    if (json_string_equals_ascii(key, utf8_literal("settings"))) {
        JsonString encoded{};
        if ((result.seen & 1) || !json_value_as_string(value, &encoded) || encoded.has_escapes)
            return false;
        result.seen |= 1;
        result.encoded = encoded.encoded;
        return true;
    }
    if (json_string_equals_ascii(key, utf8_literal("required_data_version"))) {
        u64 version = 0;
        if ((result.seen & 2) || !json_value_to_u64(value, &version) || version > ~u32{0})
            return false;
        result.seen |= 2;
        result.version = static_cast<u32>(version);
        return true;
    }
    return false;
}
} // namespace
GifFavoritesWriteResult gif_favorites_write_patch(ConstSlice<u8> observed, u32 data_version,
                                                  const GifFavoriteMutation& mutation, Slice<u8> scratch,
                                                  Slice<u8> output) noexcept {
    const auto fail = [](E error) noexcept { return GifFavoritesWriteResult{0, 0, error}; };
    const ConstSlice<u8> url{reinterpret_cast<const u8*>(mutation.url.data), mutation.url.bytes};
    const ConstSlice<u8> src{reinterpret_cast<const u8*>(mutation.src.data), mutation.src.bytes};
    if (!byte_range_valid(observed) || observed.count > kGifFavoritesMaximumGroupBytes ||
        !byte_range_valid(range(scratch)) || !byte_range_valid(range(output)) || !byte_range_valid(url) ||
        !byte_range_valid(src) || !mutation.url.bytes ||
        mutation.url.bytes > kGifFavoritesMaximumGroupBytes ||
        mutation.src.bytes > kGifFavoritesMaximumGroupBytes ||
        mutation.kind > GifFavoriteMutationKind::reorder || !utf8_validate(mutation.url).valid ||
        (mutation.kind == GifFavoriteMutationKind::add &&
         (!mutation.src.bytes || !utf8_validate(mutation.src).valid ||
          (mutation.format != GifFavoriteFormat::image && mutation.format != GifFavoriteFormat::video))))
        return fail(E::input);
    const ConstSlice<u8> inputs[]{observed, url, src};
    if (byte_ranges_overlap(range(scratch), range(output)))
        return fail(E::input);
    for (const auto input : inputs)
        if (byte_ranges_overlap(input, range(scratch)) || byte_ranges_overlap(input, range(output)))
            return fail(E::input);
    Wire group{observed, {}, observed.count};
    GifFavoritesResult measured{};
    measured.error = favorites(group, 0, group.size, measured);
    if (!measured.ok())
        return fail(measured.error);
    measured.index_words = gif_favorites_index_words(measured.entry_count);
    if (scratch.count < measured.index_words * 4)
        return fail(E::capacity);
    Index index{scratch.data, measured.index_words};
    MutationPlan plan{};
    plan.records = index_group(group, index);
    plan.order = mutation.order;
    u32 maximum_order = 0;
    for (u32 bucket = 0; bucket < index.words / 2; ++bucket) {
        const u32 last = index.get(bucket * 2 + 1);
        if (!last)
            continue;
        const auto existing = indexed_entry(group, last - 1);
        maximum_order = maximum(maximum_order, existing.order);
        if (equal(group, existing, mutation.url)) {
            plan.found = true;
            plan.winning_offset = last - 1;
        }
    }
    if (mutation.kind != GifFavoriteMutationKind::add && !plan.found)
        return fail(E::missing);
    if (mutation.kind == GifFavoriteMutationKind::add) {
        if (!plan.found && measured.entry_count == kGifFavoritesMaximumEntries)
            return fail(E::capacity);
        if (maximum_order == ~u32{0})
            return fail(E::order_overflow);
        plan.order = maximum_order + 1;
        plan.set_hide_tooltip = !measured.hide_tooltip && plan.records + (plan.found ? 0 : 1) > 2;
    }
    BinaryWriter counted{};
    write_group(group, mutation, plan, counted);
    if (counted.used > kGifFavoritesMaximumGroupBytes)
        return fail(E::capacity);
    BinaryWriter prefix{};
    prefix.number(18);
    prefix.number(counted.used);
    const u32 proto_bytes = prefix.used + counted.used;
    constexpr char start[] = "{\"settings\":\"";
    constexpr char middle[] = "\",\"required_data_version\":";
    const u32 encoded_bytes = base64_encoded_size(proto_bytes);
    constexpr u32 start_bytes = static_cast<u32>(sizeof(start) - 1);
    constexpr u32 middle_bytes = static_cast<u32>(sizeof(middle) - 1);
    const u32 json_bytes = start_bytes + encoded_bytes + middle_bytes + decimal_digits(data_version) + 1;
    if (proto_bytes > scratch.count || json_bytes > output.count ||
        json_bytes > kGifFavoritesMaximumPatchBytes)
        return fail(E::capacity);
    BinaryWriter encoded{scratch.data, 0};
    encoded.number(18);
    encoded.number(counted.used);
    write_group(group, mutation, plan, encoded);
    copy_bytes(output.data, start, start_bytes);
    base64_encode({scratch.data, encoded.used},
                  {reinterpret_cast<char*>(output.data + start_bytes), encoded_bytes});
    ProtocolWriter json{output, start_bytes + encoded_bytes};
    protocol_writer_append_literal(&json, middle);
    protocol_writer_append_u64(&json, data_version);
    protocol_writer_append_byte(&json, '}');
    return {json.used, counted.used, E::none};
}
GifFavoritesError gif_favorites_validate_patch_json(Utf8View input) noexcept {
    if (input.bytes > kGifFavoritesMaximumPatchBytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(input.data), input.bytes}))
        return E::input;
    u32 structurals[16]{};
    const auto scan = json_scan_structurals_avx1(input, {structurals, 16});
    PatchJson parsed{};
    if (!scan.ok() ||
        !json_visit_root_object(input, {structurals, scan.count}, 2, patch_member, &parsed).ok() ||
        parsed.seen != 3)
        return E::envelope;
    Wire w{};
    E error{};
    if (!encoded_wire(parsed.encoded, &w, &error))
        return error;
    u32 p = 0, f = 0, t = 0, stop = 0;
    if (!tag(w, p, w.size, f, t) || f != 2 || t != 2 || !length(w, p, w.size, stop) || stop != w.size)
        return E::envelope;
    if (stop - p > kGifFavoritesMaximumGroupBytes)
        return E::capacity;
    GifFavoritesResult group{};
    return favorites(w, p, stop, group);
}
} // namespace maicord
